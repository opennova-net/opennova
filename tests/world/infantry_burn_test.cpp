// Burn duration, availability, and damage/selection integration.
// [orig: 0x4AF4A0, 0x4B70D9, 0x4BBF8F, 0x4EB1D2]
#include <runtime/world/infantry_burn.h>
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

    apply_infantry_burn(world, entity, 1, {-1, 0, 0}, owner);
    CHECK(inf.burn_state == 1 && inf.combat_move_timer == 36);
    apply_infantry_burn(world, entity, 2, {0, -1, 0}, owner);
    CHECK(inf.burn_state == 2 && inf.combat_move_timer == 48);
    world.registry.get(owner)->group_id = 2;
    apply_infantry_burn(world, entity, 3, {-1, 0, 0}, owner);
    CHECK(inf.burn_state == 3 && inf.combat_move_timer == 1);
    apply_infantry_burn(world, entity, 4, {0, -1, 0}, owner);
    CHECK(inf.burn_state == 4 && inf.combat_move_timer == 2);
    apply_infantry_burn(world, entity, 0, {}, owner);
    CHECK(inf.burn_state == 4 && inf.combat_move_timer == 2);
    entity.engine_flags |= kEntityFlagDead;
    apply_infantry_burn(world, entity, 1, {}, owner);
    CHECK(inf.burn_state == 4);
    entity.engine_flags = kEntityFlagPlayer;
    apply_infantry_burn(world, entity, 1, {}, owner);
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

    // Blast burn precedes the damage callback's armor rejection; outside the
    // radius, neither damage nor burn occurs.
    world.tables.ammo.entries.resize(1);
    auto &ammo = world.tables.ammo.entries[0];
    ammo.valid = true;
    ammo.kztype = ammo_kz::kStandard;
    ammo.kz_damage = 100;
    ammo.kz_maxradius = 4;
    ammo.secondary_anim = 4;
    entity.engine_flags = kEntityFlagIndestructible;
    entity.health = 100;
    inf.burn_state = 0;
    ExplosionEntry blast;
    blast.type = ammo.kztype;
    blast.ammo_index = 0;
    blast.owner = owner;
    blast.pos = {1, 0, 0};
    world.explosions.queue_explosion(world, blast);
    world.explosions.process(world, nullptr, nullptr, -1000, world.out.destruction);
    CHECK(inf.burn_state == 4 && entity.health == 100);
    inf.burn_state = 0;
    blast.pos = {6, 0, 0};
    world.explosions.queue_explosion(world, blast);
    world.explosions.process(world, nullptr, nullptr, -1000, world.out.destruction);
    CHECK(inf.burn_state == 0);
    return 0;
}
