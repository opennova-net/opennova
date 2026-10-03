// Entity_ApplyCollisionForce: the burn half (duration, availability, selection
// integration) and the force half (the four kz_physics modes + the local-player
// hit blackout). [orig: 0x4AF4A0, 0x4B70D9, 0x4BBF8F, 0x4EB1D2]
#include <runtime/world/collision_force.h>
#include <runtime/world/dir_table.h>
#include <runtime/world/world.h>
#include <cstdio>
#include <memory>
using namespace opennova::world;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); return 1; } } while (0)
struct Motion final : IRootMotionSource {
    int missing = -1;
    bool has_clip(int, int state) const override { return state != missing; }
    bool advance(int, int, int32_t &phase, RootMotionFrame &out) override { ++phase; out = {}; return true; }
    int32_t clip_length_ticks(int, int, int) const override { return 100; }
};
// The retail planar impulse: floor((scale * table_q22) / 2^22).
static int32_t impulse(int64_t scale, int32_t table_q22) {
    return static_cast<int32_t>((scale * table_q22) >> 22);
}
int main() {
    auto heap = std::make_unique<World>();
    World &world = *heap;
    world.registry.configure_pool(0, 4);
    Entity seed;
    seed.kind = EntityKind::Organic;
    seed.has_item_def = true;
    seed.item_type = 3;
    seed.health = 100;
    seed.group_id = 1;
    const auto owner = world.registry.spawn(0, seed);
    seed.group_id = 2;
    const auto target = world.registry.spawn(0, seed);
    world.ai.attach(target);
    auto &body = *world.ai.for_handle(target);
    auto &entity = *world.registry.get(target);
    auto &inf = body.inf;
    body.health = 100;
    inf.active = true;
    Motion motion;
    world.ai.root_motion = &motion;

    // --- the burn half (force 0) -------------------------------------------
    // Source {-1,0,0} is straight ahead of a heading-0 body (multiplier 3);
    // {0,-1,0} sits at +90 degrees, exactly on the 0x3FFFFFC0 edge (multiplier 4).
    apply_collision_force(world, entity, 1, 0, {-1, 0, 0}, owner);
    CHECK(inf.burn_state == 1 && inf.combat_move_timer == 36);
    apply_collision_force(world, entity, 2, 0, {0, -1, 0}, owner);
    CHECK(inf.burn_state == 2 && inf.combat_move_timer == 48);
    world.registry.get(owner)->group_id = 2;
    apply_collision_force(world, entity, 3, 0, {-1, 0, 0}, owner);
    CHECK(inf.burn_state == 3 && inf.combat_move_timer == 1);
    apply_collision_force(world, entity, 4, 0, {0, -1, 0}, owner);
    CHECK(inf.burn_state == 4 && inf.combat_move_timer == 2);
    apply_collision_force(world, entity, 0, 0, {}, owner);
    CHECK(inf.burn_state == 4 && inf.combat_move_timer == 2);
    entity.engine_flags |= kEntityFlagDead;
    apply_collision_force(world, entity, 1, 0, {}, owner);
    CHECK(inf.burn_state == 4);
    entity.engine_flags = kEntityFlagPlayer;
    apply_collision_force(world, entity, 1, 0, {}, owner);
    CHECK(inf.burn_state == 1 && inf.idle_counter == 1);
    for (uint32_t tick = 4; tick < 16; tick += 4) {
        world.ai.player_body_select(body, world, 0, tick);
        CHECK(inf.anim_state == 111 && inf.idle_counter == 1);
    }
    world.ai.player_body_select(body, world, 0, 16);
    CHECK(inf.burn_state == 1 && inf.idle_counter == 0);
    world.ai.player_body_select(body, world, 0, 32);
    CHECK(inf.burn_state == 0 && inf.anim_state == 111);
    inf.anim_state = anim_state::kIdle;
    inf.burn_state = 2;
    inf.idle_counter = 1;
    motion.missing = 112;
    world.ai.player_body_select(body, world, 0, 4);
    CHECK(inf.burn_state == 2 && inf.anim_state == anim_state::kIdle);

    entity.engine_flags = 0;
    inf.combat_move_timer = 8;
    world.ai.infantry_combat_think(body, world, 16);
    CHECK(inf.burn_state == 0); // missing NPC clip clears immediately
    motion.missing = -1;
    inf.burn_state = 3;
    inf.combat_move_timer = 8;
    inf.aim_valid = true;
    const int selected = world.ai.infantry_combat_think(body, world, 16);
    CHECK(selected == 113 && inf.combat_move_timer == 7 && inf.aim_valid);
    // A live burn also bypasses the script/gait selector in the real body tick.
    world.script.forced_animation = 115;
    world.ai.tick_infantry(body, world, 0);
    CHECK(inf.anim_state == 113 && inf.burn_state == 3 && inf.combat_move_timer == 6);
    world.script.forced_animation = 0;

    // --- the force half ------------------------------------------------------
    // The LUT rows the two source positions resolve to: bearing 0 (source
    // {-1,0,0}) and bearing 0x40000000 (source {0,-1,0}).
    int32_t c0 = 0, s0 = 0, c90 = 0, s90 = 0;
    quantized_dir(0, c0, s0);
    quantized_dir(0x40000000, c90, s90);
    CHECK(s0 == 0 && c0 > 0x3FFFF0 && s90 > 0x3FFFF0);
    world.registry.get(owner)->group_id = 1;
    inf.burn_state = 0;
    inf.combat_move_timer = 0;
    const auto reset_vel = [&inf]() { inf.vel[0] = inf.vel[1] = inf.vel[2] = 0; };

    // Force 1, live body: +5120 vertical, 9216-scaled planar kick along the
    // bearing. hit_type 0 still applies the force.
    reset_vel();
    apply_collision_force(world, entity, 0, 1, {-1, 0, 0}, owner);
    CHECK(inf.burn_state == 0);
    CHECK(inf.vel[2] == 5120 && inf.vel[0] == impulse(9216, c0) && inf.vel[1] == 0);
    // The slideDecay < 4096 gate: exactly 4096 blocks, 4095 passes.
    reset_vel();
    inf.vel[2] = 4096;
    apply_collision_force(world, entity, 0, 1, {-1, 0, 0}, owner);
    CHECK(inf.vel[2] == 4096 && inf.vel[0] == 0);
    inf.vel[2] = 4095;
    apply_collision_force(world, entity, 0, 1, {-1, 0, 0}, owner);
    CHECK(inf.vel[2] == 4095 + 5120 && inf.vel[0] == impulse(9216, c0));
    // Flags 0x40 (mounted/guarding) blocks the write.
    reset_vel();
    entity.engine_flags = kEntityFlagMounted;
    apply_collision_force(world, entity, 0, 1, {-1, 0, 0}, owner);
    CHECK(inf.vel[0] == 0 && inf.vel[1] == 0 && inf.vel[2] == 0);
    // Dead body: the smaller constants, and the burn store is skipped while the
    // force still applies.
    reset_vel();
    entity.engine_flags = kEntityFlagDead;
    apply_collision_force(world, entity, 2, 1, {-1, 0, 0}, owner);
    CHECK(inf.burn_state == 0);
    CHECK(inf.vel[2] == 2560 && inf.vel[0] == impulse(4608, c0) && inf.vel[1] == 0);

    // Force 2, drift: planar only, gated on |vx| + |vy| < 4096.
    entity.engine_flags = 0;
    reset_vel();
    apply_collision_force(world, entity, 0, 2, {0, -1, 0}, owner);
    CHECK(inf.vel[0] == impulse(9216, c90) && inf.vel[1] == impulse(9216, s90) && inf.vel[2] == 0);
    reset_vel();
    inf.vel[0] = 2048;
    inf.vel[1] = -2048;
    apply_collision_force(world, entity, 0, 2, {0, -1, 0}, owner);
    CHECK(inf.vel[0] == 2048 && inf.vel[1] == -2048);
    inf.vel[0] = 2047;
    apply_collision_force(world, entity, 0, 2, {0, -1, 0}, owner);
    CHECK(inf.vel[0] == 2047 + impulse(9216, c90) && inf.vel[1] == -2048 + impulse(9216, s90));
    reset_vel();
    entity.engine_flags = kEntityFlagDead;
    apply_collision_force(world, entity, 0, 2, {0, -1, 0}, owner);
    CHECK(inf.vel[0] == impulse(2304, c90) && inf.vel[1] == impulse(2304, s90) && inf.vel[2] == 0);

    // Force 4, direct push: (table << 10) >> 22 planar, +1024 vertical, the
    // same slideDecay gate as force 1.
    entity.engine_flags = 0;
    reset_vel();
    apply_collision_force(world, entity, 0, 4, {-1, 0, 0}, owner);
    CHECK(inf.vel[0] == static_cast<int32_t>((static_cast<int64_t>(c0) << 10) >> 22));
    CHECK(inf.vel[1] == 0 && inf.vel[2] == 1024);
    reset_vel();
    inf.vel[2] = 4096;
    apply_collision_force(world, entity, 0, 4, {-1, 0, 0}, owner);
    CHECK(inf.vel[0] == 0 && inf.vel[2] == 4096);

    // Force 3, the hit blackout: only the local player arms it.
    auto &dim = world.weather.core.hit_dim;
    const auto clear_dim = [&dim]() { dim.intensity = 0; dim.fade_rate = 0; };
    clear_dim();
    apply_collision_force(world, entity, 0, 3, {-1, 0, 0}, owner);
    CHECK(dim.intensity == 0 && dim.fade_rate == 0);
    world.cached.local_player = target;
    // Enemy (different command group): both facings resolve to rate 105.
    apply_collision_force(world, entity, 0, 3, {-1, 0, 0}, owner);
    CHECK(dim.intensity == 0xA000 && dim.fade_rate == 105);
    clear_dim();
    apply_collision_force(world, entity, 0, 3, {0, -1, 0}, owner);
    CHECK(dim.intensity == 0xA000 && dim.fade_rate == 105);
    // Friendly (same group + team) offline: facing 0x8000/93 = 352, behind 0x8000/124 = 264.
    world.registry.get(owner)->group_id = 2;
    clear_dim();
    apply_collision_force(world, entity, 0, 3, {-1, 0, 0}, owner);
    CHECK(dim.intensity == 0xA000 && dim.fade_rate == 352);
    clear_dim();
    apply_collision_force(world, entity, 0, 3, {0, -1, 0}, owner);
    CHECK(dim.intensity == 0xA000 && dim.fade_rate == 264);
    // Self hit is friendly whatever the group.
    world.registry.get(owner)->group_id = 1;
    clear_dim();
    apply_collision_force(world, entity, 0, 3, {-1, 0, 0}, target);
    CHECK(dim.fade_rate == 352);
    // A different team byte breaks the friendly match.
    world.registry.get(owner)->group_id = 2;
    world.registry.get(owner)->team = 1;
    clear_dim();
    apply_collision_force(world, entity, 0, 3, {-1, 0, 0}, owner);
    CHECK(dim.fade_rate == 105);
    world.registry.get(owner)->team = 0;
    // In session, a non-team game type (DM) makes every hit an enemy hit.
    world.rules.mp_session = true;
    MatchRules dm;
    dm.game_type = 0;
    world.match.configure(dm);
    clear_dim();
    apply_collision_force(world, entity, 0, 3, {-1, 0, 0}, owner);
    CHECK(dim.fade_rate == 105);
    // A team game type (TDM 0x10000) keeps the friendly leg ...
    MatchRules tdm;
    tdm.game_type = 0x10000u;
    world.match.configure(tdm);
    clear_dim();
    apply_collision_force(world, entity, 0, 3, {-1, 0, 0}, owner);
    CHECK(dim.fade_rate == 352);
    // ... unless the rules word carries NoFriendlyFire (0x200).
    world.rules.no_friendly_fire = true;
    clear_dim();
    apply_collision_force(world, entity, 0, 3, {-1, 0, 0}, owner);
    CHECK(dim.intensity == 0 && dim.fade_rate == 0);
    // NoFriendlyFire only matters in session.
    world.rules.mp_session = false;
    apply_collision_force(world, entity, 0, 3, {-1, 0, 0}, owner);
    CHECK(dim.fade_rate == 352);
    world.rules.no_friendly_fire = false;
    world.cached.local_player = EntityHandle{};
    // An enemy's blast from here: a same-side NPC pair is a protected pair the
    // blast legs pass over (D-WPN-42).
    world.registry.get(owner)->team = 2;

    // Blast burn precedes the damage callback's armor rejection; outside the
    // radius, neither damage nor burn occurs. The blast passes kz_physics too.
    world.tables.ammo.entries.resize(1);
    auto &ammo = world.tables.ammo.entries[0];
    ammo.valid = true;
    ammo.kztype = ammo_kz::kStandard;
    ammo.kz_damage = 100;
    ammo.kz_maxradius = 4;
    ammo.secondary_anim = 4;
    ammo.kz_physics = 4;
    entity.engine_flags = kEntityFlagIndestructible;
    entity.health = 100;
    inf.burn_state = 0;
    reset_vel();
    ExplosionEntry blast;
    blast.type = ammo.kztype;
    blast.ammo_index = 0;
    blast.owner = owner;
    blast.pos = {1, 0, 0};
    world.explosions.queue_explosion(world, blast);
    world.explosions.process(world, nullptr, nullptr, -1000, world.out.destruction);
    CHECK(inf.burn_state == 4 && entity.health == 100);
    CHECK(inf.vel[2] == 1024);
    inf.burn_state = 0;
    reset_vel();
    blast.pos = {6, 0, 0};
    world.explosions.queue_explosion(world, blast);
    world.explosions.process(world, nullptr, nullptr, -1000, world.out.destruction);
    CHECK(inf.burn_state == 0 && inf.vel[2] == 0);
    return 0;
}
