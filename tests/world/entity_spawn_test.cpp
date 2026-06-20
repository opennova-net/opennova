// entity_reset_to_spawn_state — the gate-clear + position-backup half of the host
// player-spawn machine [orig: Entity_ResetToSpawnState @0x4B9610 / net-re §5.2b step 5].
#include "world/entity.h"
#include "world/entity_spawn.h"

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
    std::printf("entity_spawn: %s\n", failures == 0 ? "OK" : "FAILED");
    return failures == 0 ? 0 : 1;
}
