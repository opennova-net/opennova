// engine/runtime/world substrate tests: addressable entities (faithful find_by_net_id),
// shared var store, entity commands, tick cadence, snapshot/restore.
#include <cstdio>
#include <type_traits>
#include <utility>

#include <runtime/world/sound_emitter_mailbox.h>
#include <runtime/world/world.h>

using namespace opennova::world;

static_assert(!std::is_copy_constructible_v<World>);
static_assert(!std::is_move_constructible_v<World>);

static int failures = 0;
#define CHECK(c)                                                              \
    do {                                                                      \
        if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
    } while (0)

int main() {
    World w;
    // The weather home starts unseeded, with retail's boot BSS: a 1024 m fog
    // reference/target, the 255.0 acceleration clamp, noon, clear weather.
    // Values stay in engine units until the 0x0A encoder narrows them.
    CHECK(!w.weather.valid);
    CHECK(w.weather.fog_target_q16() == (1024 << 16));
    CHECK(w.weather.fog_accel_clamp() == 0x00FF0000u);
    CHECK(w.weather.tod_fixed24 == (12u << 24));
    CHECK(w.weather.quake_ticks == 0);
    CHECK(w.weather.cloud_scroll_rate_target == 0);
    CHECK(w.weather.rain_pct_current_q16() == 0);
    CHECK(w.weather.overcast_blend_q16() == 0);
    CHECK(w.weather.precipitation_kind == 0);
    CHECK(w.weather.generation == 0);
    CHECK(w.weather.command_generation == 0);

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
    // groupalive/groupdead read the trigger group's live count, which the
    // 62-tick rescan rebuilds [orig: WacCmd_GroupAlive @0x4ED1CC;
    // EntityPool_RecountLiveByGroup @0x40E8D0].
    Entity g1; g1.net_id = 201; g1.group_id = 7; g1.alive = true; g1.health = 50;
    w.registry.spawn(0, g1);
    Entity g2; g2.net_id = 202; g2.group_id = 7; g2.alive = true; g2.health = 50;
    w.registry.spawn(0, g2);
    w.recount_group_live();
    CHECK(w.commands.group_alive(7));
    CHECK(w.commands.kill_group(7) == 2);
    w.recount_group_live();
    CHECK(w.commands.group_dead(7));

    // shared var store.
    w.script.vars.set_mission(5, 42);
    CHECK(w.script.vars.get_mission(5) == 42);
    w.script.vars.set_mission(256, 17); // the first compiler-declared slot (0xC6B640)
    w.script.vars.set_mission(511, 18); // the last one
    w.script.vars.set_global(3, -7);
    CHECK(w.script.vars.get_global(3) == -7);
    // The per-load clear is V0..V255 only [orig: WacScript_InitAndLoad memset
    // 0x400 @0x4f95ee]; the declared half and the globals survive it.
    w.script.vars.clear_numbered_mission_vars();
    CHECK(w.script.vars.get_mission(5) == 0);
    CHECK(w.script.vars.get_mission(256) == 17);
    CHECK(w.script.vars.get_mission(511) == 18);
    CHECK(w.script.vars.get_global(3) == -7); // globals survive mission clear
    // The carry into a rebuilt store copies ONLY the declared half.
    {
        ScriptVarStore fresh;
        fresh.set_mission(5, 1);
        fresh.set_global(3, 1);
        fresh.carry_declared_from(w.script.vars);
        CHECK(fresh.get_mission(256) == 17);
        CHECK(fresh.get_mission(511) == 18);
        CHECK(fresh.get_mission(5) == 1);
        CHECK(fresh.get_global(3) == 1);
    }

    w.script.vars.clear_all();
    CHECK(w.script.vars.get_global(3) == 0); // clear_all wipes globals too

    // Out-of-range safety: reads are 0, writes are ignored (the snapshot
    // bindings loop bank-sized and rely on these exact guards).
    CHECK(w.script.vars.get_mission(-1) == 0);
    CHECK(w.script.vars.get_mission(ScriptVarStore::kMissionVars) == 0);
    w.script.vars.set_mission(ScriptVarStore::kMissionVars, 9);
    CHECK(w.script.vars.get_mission(ScriptVarStore::kMissionVars) == 0);
    w.script.vars.set_global(-1, 9); // must not crash or wrap
    CHECK(w.script.vars.get_global(-1) == 0);

    // logic tick = the 62 Hz engine tick; one call advances it by one. (The 62-tick
    // WAC divider lives inside WacSystem, where the original keeps it — see
    // wac_behavior_test.)
    w.run_logic_tick();
    CHECK(w.logic_tick == 1);

    // The entity-update counter counts completed entity updates: a gameplay
    // tick's update adds one, a pre-round tick (no entity update) adds none,
    // and a restore keeps it (it is not in the Snapshot).
    // [orig: dword_24C1948, `add dword_24C1948,esi` in
    //  Entity_UpdateAllEntities @0x4C2639]
    {
        World cw;
        CHECK(cw.entity_update_counter == 0);
        cw.run_logic_tick(true);
        CHECK(cw.logic_tick == 1 && cw.entity_update_counter == 1);
        cw.run_logic_tick(true, TickPhase::PreRound);
        CHECK(cw.logic_tick == 2 && cw.entity_update_counter == 1);
        const World::Snapshot snap = cw.snapshot();
        cw.run_logic_tick(true);
        CHECK(cw.logic_tick == 3 && cw.entity_update_counter == 2);
        cw.restore(snap);
        CHECK(cw.logic_tick == 2 && cw.entity_update_counter == 2);
    }

    // Persistent sound intents use a bounded latest-value mailbox. A host with
    // no audio presenter can run indefinitely without accumulating one string-
    // owning row per vehicle per tick; a keyed refresh keeps its producer clock.
    SoundEmitterMailbox emitter_mailbox;
    for (uint32_t tick = 1; tick <= 10000; ++tick) {
        SoundEmitterEvent event;
        event.source_spawn_id = 55;
        event.lane = 0;
        event.emitted_tick = tick;
        event.lifetime_ticks = 30;
        event.pitch_q16 = 0x10000;
        event.volume_q8_8 = 0xFFFF;
        event.set_name = "V_TRUCK_ILP";
        CHECK(emitter_mailbox.publish(std::move(event)));
    }
    CHECK(emitter_mailbox.size() == 1);
    CHECK(emitter_mailbox.pending()[0].emitted_tick == 10000);

    // Distinct live keys fill only the retail-sized admission table; keyed
    // refresh remains accepted at capacity and expired rows make room again.
    emitter_mailbox.clear();
    for (size_t i = 0; i < SoundEmitterMailbox::kCapacity; ++i) {
        SoundEmitterEvent event;
        event.source_spawn_id = i + 1;
        event.lane = 10;
        event.emitted_tick = 100;
        event.lifetime_ticks = 30;
        event.pitch_q16 = 0x10000;
        event.volume_q8_8 = 0xFFFF;
        CHECK(emitter_mailbox.publish(std::move(event)));
    }
    SoundEmitterEvent overflow;
    overflow.source_spawn_id = SoundEmitterMailbox::kCapacity + 1;
    overflow.lane = 10;
    overflow.emitted_tick = 100;
    overflow.lifetime_ticks = 30;
    overflow.pitch_q16 = 0x10000;
    overflow.volume_q8_8 = 0xFFFF;
    CHECK(!emitter_mailbox.publish(std::move(overflow)));
    SoundEmitterEvent refresh;
    refresh.source_spawn_id = 1;
    refresh.lane = 10;
    refresh.emitted_tick = 101;
    refresh.lifetime_ticks = 30;
    refresh.pitch_q16 = 0x10000;
    refresh.volume_q8_8 = 0xFFFF;
    CHECK(emitter_mailbox.publish(std::move(refresh)));
    CHECK(emitter_mailbox.size() == SoundEmitterMailbox::kCapacity);

    // Clears and source-position updates address already-live mixer state and
    // therefore cannot fail merely because allocation intents filled the
    // transport. They displace allocations while preserving the hard bound.
    SoundEmitterEvent clear;
    clear.source_spawn_id = 9001;
    clear.lane = 20;
    clear.emitted_tick = 101;
    clear.lifetime_ticks = 30;
    CHECK(emitter_mailbox.publish(std::move(clear)));
    SoundEmitterEvent saturated_anchor;
    saturated_anchor.source_spawn_id = 9002;
    saturated_anchor.source_only = true;
    saturated_anchor.emitted_tick = 101;
    saturated_anchor.lifetime_ticks = 30;
    CHECK(emitter_mailbox.publish(std::move(saturated_anchor)));
    CHECK(emitter_mailbox.size() == SoundEmitterMailbox::kCapacity);
    bool found_clear = false;
    bool found_anchor = false;
    for (const SoundEmitterEvent &event : emitter_mailbox.pending()) {
        found_clear |= event.source_spawn_id == 9001 && !event.source_only &&
                       event.lane == 20 && event.pitch_q16 == 0 &&
                       event.volume_q8_8 == 0;
        found_anchor |= event.source_spawn_id == 9002 && event.source_only;
    }
    CHECK(found_clear);
    CHECK(found_anchor);
    emitter_mailbox.prune(132);
    CHECK(emitter_mailbox.empty());

    // Source-anchor updates are independently coalesced from lane controls.
    SoundEmitterEvent lane;
    lane.source_spawn_id = 88;
    lane.lane = 0;
    lane.emitted_tick = 200;
    lane.lifetime_ticks = 30;
    lane.pitch_q16 = 0x10000;
    lane.volume_q8_8 = 0xFFFF;
    CHECK(emitter_mailbox.publish(std::move(lane)));
    SoundEmitterEvent anchor;
    anchor.source_spawn_id = 88;
    anchor.source_only = true;
    anchor.emitted_tick = 200;
    anchor.lifetime_ticks = 30;
    CHECK(emitter_mailbox.publish(std::move(anchor)));
    CHECK(emitter_mailbox.size() == 2);
    const std::vector<SoundEmitterEvent> drained = emitter_mailbox.drain();
    CHECK(drained.size() == 2);
    CHECK(emitter_mailbox.empty());

    // snapshot / restore (editor play).
    w.script.vars.set_mission(1, 7);
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
    w.script.wac_values.accuracy_spread = 3;
    w.weather.valid = true;
    w.weather.core.scalar_channels.fog_dist_target_fp = 380 << 16;
    w.weather.core.scalar_channels.fog_step_fp = 0x00123456;
    w.weather.tod_fixed24 = 0x01234567u;
    w.weather.quake_ticks = 17;
    w.weather.cloud_scroll_rate_target = 15u << 10;
    w.weather.core.scalar_channels.rain_pct_fp = 0x00008000;
    w.weather.core.scalar_channels.overcast_fp = 0x00004000;
    w.weather.precipitation_kind = 0x89ABCDEFu;
    w.weather.generation = 9;
    w.weather.precipitation.slots[5].z = 0x12340000;
    MatchRules baseline_match_rules;
    baseline_match_rules.game_type = 0x10020u;
    baseline_match_rules.score_limit = 7;
    w.match.configure(baseline_match_rules);
    w.match.upsert_player({blast_victim, 7, "Baseline"});
    World::Snapshot snap = w.snapshot();
    const EntityHandle post_snapshot = w.registry.spawn(0, blast_target);
    CHECK(post_snapshot.valid());
    const uint64_t post_snapshot_spawn_id =
            w.registry.get(post_snapshot)->registry_spawn_id;
    w.script.vars.set_mission(1, 99);
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
    w.tables.ammo.entries.push_back(blast);
    ExplosionEntry queued;
    queued.pos = w.registry.get(blast_victim)->position;
    queued.type = ammo_kz::kStandard;
    queued.ammo_index = 0;
    w.explosions.queue_explosion(w, queued);
    DeathPiece &piece = w.death_pieces.alloc();
    piece.active = true;
    piece.settled = true;
    w.out.destruction.effects.push_back({});
    w.out.destruction.sounds.push_back({});
    w.out.destruction.husk_swaps.push_back({});
    w.out.destruction.debris_triangles = 11;
    w.out.destruction.glass_points = 5;
    w.out.destruction.explosions_processed = 7;
    w.out.destruction.items_destroyed = 3;
    // CachedFrameState is transient, not part of Snapshot. A local player
    // spawned after the baseline may reuse a baseline actor's packed slot;
    // restore must not keep treating that restored actor as the local avatar.
    w.cached.local_player = post_snapshot;
    w.cached.local_health = 77;
    w.cached.humans = 2;
    w.script.wac_values.accuracy_spread = 9;
    w.weather = WeatherState{};
    w.match.remove_player(w, blast_victim);
    w.process_round_end(2);
    w.restore(snap);
    CHECK(w.script.vars.get_mission(1) == 7);
    CHECK(w.script.wac_values.accuracy_spread == 3);
    CHECK(w.weather.valid);
    CHECK(w.weather.fog_target_q16() == (380 << 16));
    CHECK(w.weather.fog_accel_clamp() == 0x00123456u);
    CHECK(w.weather.tod_fixed24 == 0x01234567u);
    CHECK(w.weather.quake_ticks == 17);
    CHECK(w.weather.cloud_scroll_rate_target == (15u << 10));
    CHECK(w.weather.rain_pct_current_q16() == 0x00008000u);
    CHECK(w.weather.overcast_blend_q16() == 0x00004000u);
    CHECK(w.weather.precipitation_kind == 0x89ABCDEFu);
    CHECK(w.weather.generation == 9);
    CHECK(w.weather.precipitation.slots[5].z == 0x12340000);
    CHECK(w.match.rules().game_type == 0x10020u);
    CHECK(w.match.rules().score_limit == 7);
    CHECK(w.match.player(blast_victim) != nullptr);
    CHECK(!w.match.outcome().ended);
    CHECK(w.round_sim.active_count == 0);
    CHECK(!w.round_sim.rounds[0].active);
    CHECK(w.round_sim.deaths.empty());
    CHECK(w.round_sim.impacts.empty());
    CHECK(w.round_sim.next_impact_order == 1);
    CHECK(w.explosions.queue.empty());
    CHECK(w.death_pieces.cursor == 0);
    for (const DeathPiece &restored_piece : w.death_pieces.pieces)
        CHECK(!restored_piece.active && !restored_piece.settled);
    CHECK(w.out.destruction.effects.empty());
    CHECK(w.out.destruction.sounds.empty());
    CHECK(w.out.destruction.husk_swaps.empty());
    CHECK(w.out.destruction.debris_triangles == 0);
    CHECK(w.out.destruction.glass_points == 0);
    CHECK(w.out.destruction.explosions_processed == 0);
    CHECK(w.out.destruction.items_destroyed == 0);
    CHECK(w.cached.local_player == blast_victim);
    CHECK(w.cached.local_health == 0);
    CHECK(w.cached.humans == 0);
    const EntityHandle post_restore = w.registry.spawn(0, blast_target);
    CHECK(post_restore == post_snapshot);
    CHECK(w.registry.get(post_restore)->registry_spawn_id >
          post_snapshot_spawn_id);
    const int32_t restored_health = w.registry.get(blast_victim)->health;
    w.run_logic_tick(true);
    CHECK(w.registry.get(blast_victim)->health == restored_health);

    // Snapshot completeness: every field World::Snapshot carries round-trips
    // through snapshot()/restore(). A member added to Snapshot without its
    // restore line, or moved out of the snapshot set by a regrouping, fails
    // here field by field (ADR 0043 slice E5 pins the set world.cpp lists).
    {
        World sw;
        sw.registry.configure_pool(0, 4);
        Entity seed;
        seed.kind = EntityKind::Organic;
        seed.alive = true;
        seed.health = 40;
        const EntityHandle kept = sw.registry.spawn(0, seed);
        sw.script.vars.set_mission(2, 11);
        sw.script.wac_values.accuracy_spread = 5;
        sw.env.time_of_day = 9 << 16;
        sw.weather.tod_fixed24 = 0x0ABCDEF0u;
        MatchRules snap_rules;
        snap_rules.game_type = 0x10001u;
        snap_rules.score_limit = 3;
        sw.match.configure(snap_rules);
        ZoneCaptureState::Request req;
        req.zone = kept;
        req.team = 2;
        sw.zones.capture.requests.push_back(req);
        sw.zones.spawn_cycle_counter = 6;
        sw.logic_tick = 700;
        sw.preround_delay_seconds = 4;
        sw.prng16_state = 0x1234u;
        sw.crt_rand.state = 77u;
        sw.cached.local_player = kept;
        const World::Snapshot sealed = sw.snapshot();

        sw.registry.get(kept)->health = 1;
        sw.script.vars.set_mission(2, 0);
        sw.script.wac_values.accuracy_spread = 0;
        sw.env.time_of_day = 0;
        sw.weather.tod_fixed24 = 0;
        MatchRules other_rules;
        other_rules.game_type = 0x10020u;
        sw.match.configure(other_rules);
        sw.zones.capture.clear();
        sw.zones.spawn_cycle_counter = 0;
        sw.logic_tick = 0;
        sw.preround_delay_seconds = 0;
        sw.prng16_state = 1u;
        sw.crt_rand.state = 1u;
        sw.cached.local_player = EntityHandle{};
        sw.restore(sealed);

        CHECK(sw.registry.get(kept) != nullptr && sw.registry.get(kept)->health == 40);
        CHECK(sw.script.vars.get_mission(2) == 11);
        CHECK(sw.script.wac_values.accuracy_spread == 5);
        CHECK(sw.env.time_of_day == (9 << 16));
        CHECK(sw.weather.tod_fixed24 == 0x0ABCDEF0u);
        CHECK(sw.match.rules().game_type == 0x10001u);
        CHECK(sw.match.rules().score_limit == 3);
        CHECK(sw.zones.capture.requests.size() == 1 &&
              sw.zones.capture.requests[0].team == 2);
        CHECK(sw.zones.spawn_cycle_counter == 6);
        CHECK(sw.logic_tick == 700);
        CHECK(sw.preround_delay_seconds == 4);
        CHECK(sw.prng16_state == 0x1234u);
        CHECK(sw.crt_rand.state == 77u);
        CHECK(sw.cached.local_player == kept);
    }

    // A SCRIPTED group kill has to reach the wire. Retail never fans deaths from
    // the damage pass: each motor's per-entity update carries the edge
    // `Health <= 0 && (Flags & 2) == 0` and notifies there, so the script's
    // KillGroup is noticed exactly like a bullet. And because the killer lives on
    // the VICTIM (entity+704) and the script never stamps it, retail's own
    // baseline capture shows those deaths with killerSource=0 - a burst of
    // consecutive slots with no killer. This pins both halves.
    {
        World kw;
        kw.registry.configure_pool(0, 8);
        // Organic rows: the edge that raises the death transaction is the
        // infantry motor's [orig: @0x4B9D4D / @0x4B4CEA].
        Entity a; a.net_id = 900; a.group_id = 9; a.alive = true; a.health = 150;
        a.kind = EntityKind::Organic;
        // A prior non-lethal hit's shooter on the victim's +0x178: the script
        // death reports it (GameEvent_PlayerDeath reads the victim's word).
        a.last_attacker = EntityHandle::make(0, 6);
        Entity b; b.net_id = 901; b.group_id = 9; b.alive = true; b.health = 150;
        b.kind = EntityKind::Organic;
        const EntityHandle ha = kw.registry.spawn(0, a);
        kw.registry.spawn(0, b);
        CHECK(kw.round_sim.deaths.empty());
        CHECK(kw.commands.kill_group(9) == 2);
        CHECK(kw.round_sim.deaths.size() == 2);
        if (kw.round_sim.deaths.size() == 2) {
            CHECK(kw.round_sim.deaths[0].victim_handle == ha.packed);
            // Unstamped killer - the signature that separates a scripted kill
            // from a shot one in the capture.
            CHECK(kw.round_sim.deaths[0].killer_handle == 0);
            CHECK(kw.round_sim.deaths[1].killer_handle == 0);
            CHECK(kw.round_sim.deaths[0].killer == EntityHandle::make(0, 6));
            CHECK(!kw.round_sim.deaths[1].killer.valid()); // never hit: unattributed
        }
        kw.recount_group_live();
        CHECK(kw.commands.group_dead(9));

        // Killing an already-dead group must not notify twice: retail's
        // `(Flags & 2) == 0` half of the edge.
        kw.round_sim.deaths.clear();
        kw.commands.kill_group(9);
        CHECK(kw.round_sim.deaths.empty());

        // Zeroing a group's health is the same edge by another name - the motor
        // only ever sees the zero.
        Entity c; c.net_id = 902; c.group_id = 11; c.alive = true; c.health = 150;
        c.kind = EntityKind::Organic;
        kw.registry.spawn(0, c);
        kw.commands.set_group_hp(11, 0);
        CHECK(kw.round_sim.deaths.size() == 1);
        if (!kw.round_sim.deaths.empty())
            CHECK(kw.round_sim.deaths[0].killer_handle == 0);
        // A non-lethal set stays silent.
        kw.round_sim.deaths.clear();
        Entity d; d.net_id = 903; d.group_id = 12; d.alive = true; d.health = 150;
        d.kind = EntityKind::Organic;
        kw.registry.spawn(0, d);
        kw.commands.set_group_hp(12, 75);
        CHECK(kw.round_sim.deaths.empty());
    }

    std::printf(failures ? "WORLD TESTS FAILED (%d)\n" : "world tests passed\n", failures);
    return failures ? 1 : 0;
}
