// The RoundSim debug trail (world::RoundDebugEvent) over the SHIPPED ammo
// table: a grenadehe spawned over nothing dies by its fuse, never by an
// impact — the case the retired RoundDebugReport binding served to GUT
// (throwable_repro_test.gd's flight-until-the-fuse witness, ADR 0043 d10).
// Asset-gated on the reference fixture set's ammo.def (a byte-exact copy of
// BASE JO: the grenadehe row's max_age is the 4 s fuse, 248 ticks).
#include <cstdio>
#include <cstring>
#include <string>

#include "common/retail_paths.h"

#include <formats/def/def.h>
#include <runtime/world/ammo_table_build.h>
#include <runtime/world/round_sim.h>
#include <runtime/world/world.h>

using namespace opennova;
using namespace opennova::world;
using namespace opennova::def;

static int failures = 0;
#define CHECK(c)                                                              \
    do {                                                                      \
        if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
    } while (0)

int main() {
    RETAIL_REQUIRE_OR_SKIP(ammo_path, retail::reference_fixture("def/ammo.def"),
            "OPENNOVA_JO_ASSETS/fixtures/def/ammo.def (the shipped ammo table)");
    DefAmmoFile ammo;
    std::memset(&ammo, 0, sizeof(ammo));
    CHECK(def_parse_ammo(ammo_path.c_str(), &ammo) == 0);
    if (failures != 0) return 1;

    World w;
    w.tables.ammo = build_ammo_table(ammo);
    def_free_ammo(&ammo);
    w.registry.configure_pool(0, 8);
    w.registry.configure_pool(1, 16);
    w.registry.configure_pool(2, 8);
    // The thrower at the origin (the retired case's local player), no terrain
    // under the flight: only the fuse can end the round.
    Entity seed;
    seed.kind = EntityKind::Organic;
    seed.team = 1;
    seed.health = 100;
    seed.alive = true;
    const EntityHandle thrower = w.registry.spawn(0, seed);
    CHECK(thrower.valid());

    const int ammo_index = w.tables.ammo.index_of("grenadehe");
    CHECK(ammo_index >= 0); // the real ammo.def grenadehe row
    if (ammo_index < 0) return 1;

    // Godot (0, 10, 0) along (1, 1, 0): mission origin (0, 0, 10), yaw 0,
    // pitch +45 degrees in BAM32 (the debug_spawn_round conversion the GUT drove).
    RoundSpawnParams p;
    p.owner = thrower;
    p.shooter_handle = thrower.packed;
    p.origin = Vec3{0.0f, 0.0f, 10.0f};
    p.dir_yaw_bam = 0;
    p.dir_pitch_bam = 0x20000000;
    p.ammo_index = ammo_index;
    const int slot = w.round_sim.spawn(w, p);
    CHECK(slot >= 0); // grenadehe spawned
    if (slot < 0) return 1;

    int expired_tick = -1;
    for (int t = 0; t < 300; ++t) {
        w.round_sim.tick(w, /*terrain=*/nullptr);
        const int count = w.round_sim.debug_trail_count;
        int idx = (w.round_sim.debug_trail_next - count + RoundSim::kDebugTrailCap * 2) %
                  RoundSim::kDebugTrailCap;
        for (int i = 0; i < count; ++i, idx = (idx + 1) % RoundSim::kDebugTrailCap) {
            const RoundDebugEvent &ev = w.round_sim.debug_trail[static_cast<size_t>(idx)];
            if (ev.kind == RoundDebugEvent::kExpired && expired_tick < 0) expired_tick = t;
        }
    }
    // The fuse, not an impact, ends the round.
    CHECK(expired_tick >= 240 && expired_tick <= 260);
    if (failures == 0) std::printf("OK: round_debug_trail (fuse at tick %d)\n", expired_tick);
    return failures == 0 ? 0 : 1;
}
