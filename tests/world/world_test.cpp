// libs/world substrate tests: addressable entities (faithful find_by_net_id),
// shared var store, entity commands, tick cadence, snapshot/restore.
#include <cstdio>

#include "world/world.h"

using namespace opennova::world;

static int failures = 0;
#define CHECK(c)                                                              \
    do {                                                                      \
        if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
    } while (0)

int main() {
    World w;
    w.registry.configure_pool(0, 16); // actor pool
    w.registry.configure_pool(1, 16); // actor pool
    w.registry.configure_pool(4, 8);  // static pool (not searched by find_by_net_id)

    Entity e;
    e.net_id = 100;
    e.kind = EntityKind::Organic;
    e.team = 2;
    e.group_id = 5;
    e.health = 100;
    e.alive = true;
    EntityHandle h = w.registry.spawn(0, e);
    CHECK(h.valid());
    CHECK(h.pool() == 0);
    CHECK(w.registry.get(h) != nullptr);

    // find_by_net_id contract.
    CHECK(w.registry.find_by_net_id(100) == h);
    CHECK(!w.registry.find_by_net_id(999).valid());

    // pool-0-first ordering: a duplicate net id in pool 1 must lose to pool 0.
    Entity dup;
    dup.net_id = 100;
    w.registry.spawn(1, dup);
    CHECK(w.registry.find_by_net_id(100).pool() == 0);

    // pool 4 is NOT searched (actor mask &0xF) — a net id only in pool 4 is absent.
    Entity p4;
    p4.net_id = 400;
    w.registry.spawn(4, p4);
    CHECK(!w.registry.find_by_net_id(400).valid());

    // commands operate on the clean model.
    CHECK(w.commands.ssn_alive(100));
    CHECK(w.commands.kill_ssn(100));
    CHECK(w.commands.ssn_dead(100));

    // group fan-out.
    Entity g1; g1.net_id = 201; g1.group_id = 7; g1.alive = true; w.registry.spawn(0, g1);
    Entity g2; g2.net_id = 202; g2.group_id = 7; g2.alive = true; w.registry.spawn(0, g2);
    CHECK(w.commands.group_alive(7));
    CHECK(w.commands.kill_group(7) == 2);
    CHECK(w.commands.group_dead(7));

    // shared var store.
    w.vars.set_mission(5, 42);
    CHECK(w.vars.get_mission(5) == 42);
    w.vars.set_global(3, -7);
    CHECK(w.vars.get_global(3) == -7);
    w.vars.clear_mission();
    CHECK(w.vars.get_mission(5) == 0);
    CHECK(w.vars.get_global(3) == -7); // globals survive mission clear

    // The music bank (M0..M15) + clear_all (the bank the snapshot bindings read).
    w.vars.set_music(2, 11);
    CHECK(w.vars.get_music(2) == 11);
    w.vars.clear_all();
    CHECK(w.vars.get_music(2) == 0);
    CHECK(w.vars.get_global(3) == 0); // clear_all wipes globals too

    // Out-of-range safety: reads are 0, writes are ignored (the snapshot
    // bindings loop bank-sized and rely on these exact guards).
    CHECK(w.vars.get_mission(-1) == 0);
    CHECK(w.vars.get_mission(ScriptVarStore::kMissionVars) == 0);
    w.vars.set_mission(ScriptVarStore::kMissionVars, 9);
    CHECK(w.vars.get_mission(ScriptVarStore::kMissionVars) == 0);
    w.vars.set_global(-1, 9); // must not crash or wrap
    CHECK(w.vars.get_global(-1) == 0);
    CHECK(w.vars.get_music(ScriptVarStore::kMusicVars) == 0);

    // logic tick = the 62 Hz engine tick; one call advances it by one. (The 62-tick
    // WAC divider lives inside WacSystem, where the original keeps it — see
    // wac_behavior_test.)
    w.run_logic_tick();
    CHECK(w.logic_tick == 1);

    // snapshot / restore (editor play).
    w.vars.set_mission(1, 7);
    Entity blast_target;
    blast_target.kind = EntityKind::Organic;
    blast_target.health = 100;
    blast_target.health_max = 100;
    blast_target.alive = true;
    blast_target.position = Vec3{20.0f, 20.0f, 1.0f};
    blast_target.bound_radius = 0.6f;
    const EntityHandle blast_victim = w.registry.spawn(0, blast_target);
    // Local ownership is mission-lifetime identity even though the other cached
    // fields are per-tick transients. A listen-server baseline may already own
    // its player, so that one handle must survive restore.
    w.cached.local_player = blast_victim;
    w.cached.local_health = 100;
    w.cached.humans = 1;
    World::Snapshot snap = w.snapshot();
    const EntityHandle post_snapshot = w.registry.spawn(0, blast_target);
    CHECK(post_snapshot.valid());
    const uint64_t post_snapshot_spawn_id =
            w.registry.get(post_snapshot)->registry_spawn_id;
    w.vars.set_mission(1, 99);
    w.commands.kill_ssn(201); // mutate after snapshot
    w.round_sim.rounds[0].active = true;
    w.round_sim.active_count = 1;
    w.round_sim.deaths.push_back({});
    w.round_sim.impacts.push_back({});
    w.round_sim.next_impact_order = 9;
    AmmoTableEntry blast;
    blast.valid = true;
    blast.kztype = ammo_kz::kStandard;
    blast.kz_damage = 25;
    blast.kz_maxradius = 4.0f;
    w.ammo.entries.push_back(blast);
    ExplosionEntry queued;
    queued.pos = w.registry.get(blast_victim)->position;
    queued.type = ammo_kz::kStandard;
    queued.ammo_index = 0;
    w.explosions.queue_explosion(w, queued);
    DeathPiece &piece = w.death_pieces.alloc();
    piece.active = true;
    piece.settled = true;
    w.destruction.effects.push_back({});
    w.destruction.sounds.push_back({});
    w.destruction.husk_swaps.push_back({});
    w.destruction.debris_bursts.push_back({});
    w.destruction.glass_breaks.push_back({});
    w.destruction.explosions_processed = 7;
    w.destruction.items_destroyed = 3;
    // CachedFrameState is transient, not part of Snapshot. A local player
    // spawned after the baseline may reuse a baseline actor's packed slot;
    // restore must not keep treating that restored actor as the local avatar.
    w.cached.local_player = post_snapshot;
    w.cached.local_health = 77;
    w.cached.humans = 2;
    w.restore(snap);
    CHECK(w.vars.get_mission(1) == 7);
    CHECK(w.round_sim.active_count == 0);
    CHECK(!w.round_sim.rounds[0].active);
    CHECK(w.round_sim.deaths.empty());
    CHECK(w.round_sim.impacts.empty());
    CHECK(w.round_sim.next_impact_order == 1);
    CHECK(w.explosions.queue.empty());
    CHECK(w.death_pieces.cursor == 0);
    for (const DeathPiece &restored_piece : w.death_pieces.pieces)
        CHECK(!restored_piece.active && !restored_piece.settled);
    CHECK(w.destruction.effects.empty());
    CHECK(w.destruction.sounds.empty());
    CHECK(w.destruction.husk_swaps.empty());
    CHECK(w.destruction.debris_bursts.empty());
    CHECK(w.destruction.glass_breaks.empty());
    CHECK(w.destruction.explosions_processed == 0);
    CHECK(w.destruction.items_destroyed == 0);
    CHECK(w.cached.local_player == blast_victim);
    CHECK(w.cached.local_health == 0);
    CHECK(w.cached.humans == 0);
    const EntityHandle post_restore = w.registry.spawn(0, blast_target);
    CHECK(post_restore == post_snapshot);
    CHECK(w.registry.get(post_restore)->registry_spawn_id >
          post_snapshot_spawn_id);
    const int32_t restored_health = w.registry.get(blast_victim)->health;
    w.run_logic_tick(true, false);
    CHECK(w.registry.get(blast_victim)->health == restored_health);

    std::printf(failures ? "WORLD TESTS FAILED (%d)\n" : "world tests passed\n", failures);
    return failures ? 1 : 0;
}
