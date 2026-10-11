#include <runtime/world/world.h>
#include <runtime/world/destruction.h>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <runtime/audio/oneshot_play.h>
#include <runtime/mission/mission_kernel.h>
#include <runtime/mission/item_traits.h>
#include <cstring>
#include "item_pool_step.h"
using namespace opennova::world;
using test_world::step_item_pool;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n",__LINE__,#c);return 1; } } while(0)
EntityHandle spawn(World &w, int pool, int id, ItemDeathClass cls) {
    Entity e;
    e.item_id = id;
    e.has_item_def = true;
    // A def row's ordinal: the ItemTypeIndex the item gates read
    // [orig: Entity_KillBySlotId @0x42BD29; Entity_ClearHealthInBounds @0x509E89].
    e.item_type_index = 7;
    e.kind = EntityKind::Item;
    const auto h = w.registry.spawn(pool, e);
    ItemDeathTraits t;
    t.death_class = cls;
    w.tables.item_death_traits.set(id, t);
    return h;
}
int test_regional_sound() {
    auto heap = std::make_unique<World>();
    auto &w = *heap;
    w.registry.configure_pool(2, 8);
    const auto h = spawn(w, 2, 10, ItemDeathClass::kEnvironmentSound);
    auto &e = *w.registry.get(h);
    auto *traits = w.tables.item_death_traits.get_mutable(10);
    w.weather.tod_fixed24 = uint32_t(12 * 65536) << 8;
    // The re-arm draw steps the inline dword_31BFBB8 LCG (the throwable fan
    // stream); PRNG_Next16's dword_31BFBB0 never moves here.
    const uint32_t seed = w.prng16_state;
    const uint32_t fan = w.throwables.fan_prng_state;
    destruction_notify_item_damage(w, e, 0);
    CHECK(w.throwables.fan_prng_state == fan && w.out.slot_sounds.empty());
    traits->regional_sounds[0] = {"Bird", 99, 0};
    traits->regional_sounds[1] = {"", 12, 65536};
    destruction_notify_item_damage(w, e, 1);
    CHECK(w.throwables.fan_prng_state == fan);
    destruction_notify_item_damage(w, e, 0); // Other-region sound still consumes one draw.
    CHECK(w.throwables.fan_prng_state != fan);
    CHECK(e.class_think_ticks == 12 + int(w.throwables.fan_prng_state & 65535));
    CHECK(w.prng16_state == seed);
    CHECK(w.out.slot_sounds.empty());
    traits->has_sound_point = true;
    traits->sound_point = {0, 0, 2};
    e.position = {3, 4, 5};
    for (const int hour : {4, 10, 17, 21}) {
        w.weather.tod_fixed24 = uint32_t(hour * 65536) << 8;
        destruction_notify_item_damage(w, e, 0);
        CHECK(w.out.slot_sounds.empty()); // Exact boundaries choose night.
    }
    w.weather.tod_fixed24 = uint32_t(4 * 65536 + 1) << 8;
    destruction_notify_item_damage(w, e, 0);
    CHECK(e.class_think_ticks == 99 && w.out.slot_sounds.size() == 1);
    CHECK(w.out.slot_sounds[0].pos[2] == 7 * 65536);
    CHECK(std::strcmp(w.out.slot_sounds[0].set_name, "Bird") == 0);
    // The SOUND point rides the scaled entity orientation matrix.
    e.uniform_scale_q16 = 2 * 65536;
    destruction_notify_item_damage(w, e, 0);
    CHECK(w.out.slot_sounds.size() == 2 && w.out.slot_sounds[1].pos[2] == 9 * 65536);
    CHECK(w.prng16_state == seed);
    const auto staggered = spawn(w, 2, 11, ItemDeathClass::kEnvironmentSound);
    w.tables.item_death_traits.get_mutable(11)->regional_sounds[0] = {"Bird", 99, 0};
    w.weather.tod_fixed24 = uint32_t(4 * 65536) << 8;
    destruction_notify_item_damage(w, *w.registry.get(staggered), 0);
    CHECK(w.out.slot_sounds.size() == 3); // Handle low nibble moves the boundary.

    opennova::lwf::File bank;
    for (const char *name : {"Male", "Female", "Explicit"}) {
        opennova::lwf::Multi set;
        set.name = name;
        bank.multis.push_back(set);
    }
    opennova::audio::SoundSetIndex sets;
    sets.add_bank(0, bank);
    w.tables.sound_sets = &sets;
    const char profiles[] = "begin P\r\n dawnshot Male .5 .25\r\n end\r\n"
                            "begin F\r\n dawnshot Female .25 .125\r\n end\r\n";
    CHECK(w.tables.sound_profiles.parse(profiles, sizeof(profiles)-1) == 2);
    const char definitions[] = "begin Env\r\n id 100010\r\n sound_profile P\r\n"
            " sound_profileFemale F\r\n dawnshot Explicit 2 3\r\n dayshot Missing 4 5\r\n end\r\n";
    opennova::def::DefItemsFile items{};
    CHECK(opennova::def::def_parse_items_memory(
            reinterpret_cast<const unsigned char *>(definitions), sizeof(definitions)-1, &items) == 0);
    opennova::mission::resolve_item_event_sounds(w, items);
    traits = w.tables.item_death_traits.get_mutable(10);
    CHECK(traits->regional_sounds[0].name == "Explicit");
    CHECK(traits->regional_sounds[0].base_ticks == 16384);
    CHECK(traits->regional_sounds[0].range_ticks == 8192);
    CHECK(traits->regional_sounds[1].name.empty());
    CHECK(traits->regional_sounds[1].base_ticks == 248);
    opennova::def::def_free_items(&items);
    return 0;
}
int test_flag_callback() {
    auto heap = std::make_unique<World>();
    auto &w = *heap;
    w.registry.configure_pool(0, 4);
    w.registry.configure_pool(1, 4);
    MatchRules rules;
    rules.flag_return_ticks = 5;
    w.match.configure(rules);
    const auto h = spawn(w, 1, 4091, ItemDeathClass::kFlag);
    auto &flag = *w.registry.get(h);
    flag.health = 0; // This class never dispatches a health death.
    destruction_notify_item_damage(w, flag, 0);
    flag.position = {2, 0, 0};
    destruction_notify_item_damage(w, flag, 0); // Exactly two units is away.
    for (int visit = 0; visit < 4; ++visit) {
        destruction_notify_item_damage(w, flag, 0);
        CHECK(flag.position.x == 2);
    }
    destruction_notify_item_damage(w, flag, 0);
    CHECK(flag.position.x == 0 && flag.engine_flags == 0);
    flag.position = {1.99f, 0, 1.99f};
    for (int visit = 0; visit < 7; ++visit) destruction_notify_item_damage(w, flag, 0);
    CHECK(flag.position.x == 1.99f); // Near home keeps rearming.
    Entity body;
    body.team = 1;
    body.position = {7, 8, 9};
    const auto carrier = w.registry.spawn(0, body);
    w.cached.local_player = carrier;
    flag.team = 1;
    flag.primary_occupant = carrier;
    destruction_notify_item_damage(w, flag, 0);
    CHECK(flag.position.x == 7 && flag.position.y == 8 && flag.position.z == 1.99f);
    flag.team = 2;
    flag.position.x = 3;
    destruction_notify_item_damage(w, flag, 0);
    CHECK(flag.position.x == 3); // Only a local teammate gets the XY copy.
    w.rules.logic_authority = false;
    destruction_notify_item_damage(w, flag, 4);
    CHECK(flag.class_think_ticks == 0x1000000 && flag.engine_flags == 0);
    return 0;
}
int test_building_collapse() {
    auto heap = std::make_unique<World>();
    auto &w = *heap;
    w.registry.configure_pool(2, 8);
    const auto h = spawn(w, 2, 20, ItemDeathClass::kCollapsingBuilding);
    auto &entity = *w.registry.get(h);
    auto *traits = w.tables.item_death_traits.get_mutable(20);
    traits->model_loaded = traits->model_bounds_loaded = true;
    traits->model_radius_q16 = 12 * 65536;
    traits->model_min_q16[0] = -6 * 65536;
    traits->model_max_q16[0] = 6 * 65536;
    traits->model_min_q16[1] = -3 * 65536;
    traits->model_max_q16[1] = 3 * 65536;
    traits->particledeath = "Dust";
    traits->sound_death = "Collapse";
    entity.yaw = 90; // Identity in the mission placement frame.
    entity.position = {2, 4, 6};
    entity.uniform_scale_q16 = 4 * 65536; // Perimeter ignores entity scale.
    destruction_notify_item_damage(w, entity, 0);
    CHECK(entity.class_think_ticks == 1920);
    entity.health = 0;
    destruction_notify_item_damage(w, entity, 1);
    CHECK(entity.collapse_step == 0 && entity.class_think_ticks == 16);
    CHECK(w.out.destruction.sounds.size() == 1 && w.out.destruction.effects.empty());
    w.logic_tick = 24;
    destruction_notify_item_damage(w, entity, 0);
    CHECK(entity.collapse_step == 1 && entity.class_think_ticks == 8);
    CHECK(w.out.destruction.effects.size() == 8);
    CHECK(w.out.destruction.effects[0].pos.x == -4);
    CHECK(w.out.destruction.effects[0].pos.y == 1);
    CHECK(w.out.destruction.effects[0].pos.z == 6);
    for (int step = 1; step < 64; ++step) {
        w.logic_tick += 16;
        destruction_notify_item_damage(w, entity, 0);
    }
    CHECK(entity.collapse_step == 64 && entity.class_think_ticks == 8);
    CHECK(w.out.terrain_scorches.record_count() == 1);
    CHECK(w.out.terrain_scorches.pending()[0].mission_bounds.minimum_x_q16 == -10 * 65536);
    CHECK(w.out.terrain_scorches.pending()[0].mission_bounds.maximum_x_q16 == 14 * 65536);
    destruction_notify_item_damage(w, entity, 0);
    CHECK(entity.class_think_ticks == 62);
    CHECK(w.out.destruction.effects.size() == 8);
    const ItemDeathTraits saved_traits = *traits;
    const auto remote_handle = spawn(w, 2, 20, ItemDeathClass::kCollapsingBuilding);
    // spawn's test helper replaces traits, so restore the existing row.
    w.tables.item_death_traits.set(20, saved_traits);
    auto &remote = *w.registry.get(remote_handle);
    w.rules.logic_authority = false;
    destruction_notify_item_damage(w, remote, 4);
    CHECK(remote.health == 0 && remote.collapse_step == 1);
    CHECK(remote.death_tick == w.logic_tick && remote.class_think_ticks == 8);
    remote.death_anim_state = 17;
    remote.last_attacker = h;
    remote.health = 10;
    const size_t sounds = w.out.destruction.sounds.size();
    apply_item_state_event(w, remote, -1);
    CHECK(remote.health == 0 && remote.death_anim_state == 17 && remote.last_attacker == h);
    CHECK(remote.collapse_step == 1 && w.out.destruction.sounds.size() == sounds);
    return 0;
}
int test_crane_pair_and_fade() {
    auto heap = std::make_unique<World>();
    auto &w = *heap;
    w.registry.configure_pool(2, 8);
    const auto base_handle = spawn(w, 2, 30, ItemDeathClass::kCrane);
    const auto half_handle = spawn(w, 2, 31, ItemDeathClass::kCrane);
    auto &base = *w.registry.get(base_handle);
    auto &half = *w.registry.get(half_handle);
    auto *base_traits = w.tables.item_death_traits.get_mutable(30);
    auto *half_traits = w.tables.item_death_traits.get_mutable(31);
    for (auto *t : {base_traits, half_traits}) {
        t->model_loaded = t->model_bounds_loaded = true;
        t->model_radius_q16 = 10 * 65536;
    }
    base_traits->graphic_name = "ScRaNe";
    half_traits->graphic_name = "scrane2";
    base_traits->model_section0_min_z_q16 = -5 * 65536;
    base_traits->model_section0_max_z_q16 = 2 * 65536;
    base.position = {4, 5, 0};
    half.position = {4, 5, 12};
    destruction_notify_item_damage(w, base, 0);
    CHECK(base.class_think_ticks == 1860);
    CHECK(base.attach_parent == half_handle && half.attach_parent == base_handle);
    base.health = 0;
    destruction_notify_item_damage(w, base, 1);
    CHECK(half.death_motion == DeathMotionMode::CraneFalling && half.health > 0);
    // The pool-2 update callback runs only on the slot's cohort visit (slot 1:
    // tick & 7 == 1), so the half steps once per eight ticks, never per tick.
    for (int tick = 2; tick <= 33; ++tick) {
        w.logic_tick = tick;
        tick_item_event_pool(w, 2);
        if (tick == 8) CHECK(half.position.z == 12 && half.veh.slide_z == 0);
        if (tick == 9) CHECK(half.position.z == 11 && half.veh.slide_z == -65536);
        if (tick == 16) CHECK(half.position.z == 11 && half.veh.slide_z == -65536);
        if (tick == 17) CHECK(half.position.z == 9 && half.veh.slide_z == -2 * 65536);
    }
    CHECK(half.position.z == 3 && half.health == -1);
    CHECK(half.death_motion == DeathMotionMode::BuildingEffects);
    CHECK(half.veh.slide_z == -4 * 65536 && half.class_think_ticks == 0);
    // Kind zero at crane collapse emits no scorch, even with a valid radius.
    base.collapse_step = 8;
    base.death_tick = 0;
    destruction_notify_item_damage(w, base, 0);
    CHECK(base.death_tick == 33 && w.out.terrain_scorches.record_count() == 0);
    // upfx's late blasts wait for the delay and all five fade phases.
    half.engine_flags = kEntityFlagDead | kEntityFlagHusk;
    half.item_type = 5;
    half.death_tick = 10;
    half.destroy_timer = 20;
    half_traits->primary_husk_loaded = true;
    half_traits->destroy_timing_ticks[1] = 10;
    half_traits->destroy_timing_ticks[2] = 5;
    half_traits->kz = 4;
    w.tables.ammo.entries.resize(2);
    w.tables.ammo.entries[0].name = "kz_OrganicBlast";
    w.tables.ammo.entries[1].name = "kz_MItemBlast";
    for (auto &ammo : w.tables.ammo.entries) {
        ammo.valid = true;
        ammo.kztype = ammo_kz::kRadiusBlast;
    }
    w.logic_tick = 29;
    CHECK(tick_item_class_motion(w, half, nullptr));
    CHECK(half.destroy_timer == 20 && w.explosions.queue.empty());
    w.logic_tick = 30;
    tick_item_class_motion(w, half, nullptr);
    CHECK(half.destroy_timer == 0 && half.death_tick == 30);
    w.logic_tick = 59;
    tick_item_class_motion(w, half, nullptr);
    CHECK(w.explosions.queue.empty());
    w.logic_tick = 60;
    w.rules.logic_authority = false;
    tick_item_class_motion(w, half, nullptr);
    CHECK(w.explosions.queue.empty() && (half.sub_type & 0x80) == 0);
    w.rules.logic_authority = true;
    tick_item_class_motion(w, half, nullptr);
    CHECK(w.explosions.queue.size() == 2 && (half.sub_type & 0x80) != 0);
    CHECK(w.explosions.queue[0].owner == half_handle && w.explosions.queue[0].radius_override == 4);
    tick_item_class_motion(w, half, nullptr);
    CHECK(w.explosions.queue.size() == 2);
    return 0;
}
int test_emitter_lifetime() {
    namespace p = opennova::particle;
    auto heap = std::make_unique<World>();
    auto &w = *heap;
    w.registry.configure_pool(2, 8);
    const auto h = spawn(w, 2, 40, ItemDeathClass::kEmitter);
    auto &entity = *w.registry.get(h);
    auto *traits = w.tables.item_death_traits.get_mutable(40);
    traits->model_loaded = traits->has_particlefx_point = true;
    traits->particlefx = "Puff";
    traits->particlefx_point_q16[0] = 2 * 65536;
    traits->particlefx_direction_q16[2] = 65536;
    traits->regional_sounds[0].base_ticks = 40;
    entity.yaw = 90;
    entity.position = {10, 20, 30};
    entity.uniform_scale_q16 = 2 * 65536;
    auto scene = std::make_shared<p::EffectScene>();
    p::ParticleDef definition;
    definition.id = "Smoke";
    definition.emit_dur = 0.016f;
    definition.emit_burst = 1;
    definition.age = 0.016f;
    p::EffectSceneConfig config;
    config.documents.push_back({"emit.ptl", {}});
    config.documents[0].file.particles.push_back(definition);
    config.documents[0].file.effects.push_back({"Puff", {"Smoke"}});
    scene->open(config);
    w.item_emitters.bind_scene(scene);
    // The re-arm draw steps the inline dword_31BFBB8 LCG (the throwable fan
    // stream); PRNG_Next16's dword_31BFBB0 never moves here.
    const uint32_t before = w.throwables.fan_prng_state;
    const uint32_t shared = w.prng16_state;
    destruction_notify_item_damage(w, entity, 1);
    CHECK(w.throwables.fan_prng_state == before && scene->live_counts().group_count == 0);
    destruction_notify_item_damage(w, entity, 0);
    CHECK(entity.class_think_ticks == 40 && w.throwables.fan_prng_state != before);
    CHECK(w.prng16_state == shared);
    const auto group = scene->inspect(false).groups.front();
    CHECK(group.pose.position.x == 14 && group.pose.position.y == 30 && group.pose.position.z == -20);
    CHECK(group.binding == p::EffectBinding::FollowOwner);
    entity.position.x = 15;
    w.item_emitters.sync_owners(w);
    CHECK(scene->inspect(false).groups.front().pose.position.x == 19);
    // A still-live group is stopped, gets the 15-tick gap, and consumes no RNG.
    const uint32_t after_spawn = w.throwables.fan_prng_state;
    destruction_notify_item_damage(w, entity, 0);
    CHECK(entity.class_think_ticks == 15 && w.throwables.fan_prng_state == after_spawn);
    CHECK(scene->inspect(false).groups.front().detached);
    // A naturally completed group clears ownership before the next callback.
    scene->reset_runtime_state();
    destruction_notify_item_damage(w, entity, 0);
    CHECK(scene->live_counts().group_count == 1);
    for (int tick = 0; tick < 30; ++tick) scene->advance_simulation({0.016f});
    CHECK(scene->live_counts().group_count == 0);
    destruction_notify_item_damage(w, entity, 0);
    CHECK(entity.class_think_ticks == 40 && scene->live_counts().group_count == 1);
    // Destroying/reusing the registry slot detaches the old lifetime's group.
    w.registry.despawn(h);
    w.item_emitters.sync_owners(w);
    CHECK(scene->inspect(false).groups.front().detached);
    return 0;
}
struct TestPieceSpawner : IItemPieceSpawner {
    World &world;
    std::vector<EntityHandle> pieces;
    explicit TestPieceSpawner(World &w) : world(w) {}
    EntityHandle spawn_item_piece(const Entity &seed) override {
        const auto h = world.registry.spawn(2, seed);
        pieces.push_back(h);
        return h;
    }
};
int test_palm_sections() {
    auto heap = std::make_unique<World>(); auto &w = *heap;
    w.registry.configure_pool(2, 8); w.rules.mp_session = true;
    TestPieceSpawner spawner(w); w.item_piece_spawner = &spawner;
    const auto h = spawn(w, 2, 21, ItemDeathClass::kPalm);
    auto &e = *w.registry.get(h);
    e.palm_sections = true; // the render tag's psec / cesp row (the trait)
    auto *t = w.tables.item_death_traits.get_mutable(21);
    t->model_pivots_q16 = {{{0, 0, 2*65536}}, {{0, 0, 4*65536}}};
    t->sound_death = "PalmBreak";
    w.tables.item_death_traits.set(900, *t);
    e.position = {8, 16, 3}; e.yaw = 90;
    destruction_notify_item_damage(w, e, 1, {2, 100});
    CHECK(e.palm_state == 0 && spawner.pieces.empty());
    CHECK(item_hidden_sections(e) == 0x38);
    destruction_notify_item_damage(w, e, 1, {5, 1});
    CHECK(e.palm_state == 2 && e.palm_damage[1] == 101);
    CHECK(e.class_think_ticks == 32 && spawner.pieces.size() == 1);
    CHECK(item_hidden_sections(e) == 0x1C && e.health == 100);
    auto &leaf = *w.registry.get(spawner.pieces[0]);
    CHECK(leaf.palm_state == 17 && leaf.item_id == 900);
    // A fragment, not a section clone: it runs item 900's own event row
    // [orig: Projectile_SpawnFromTile @0x53C35D].
    CHECK(leaf.item_section_piece && !leaf.section_clone);
    CHECK(leaf.position.z == 7 && leaf.engine_flags == kEntityFlagDead);
    CHECK(leaf.health == 20 && leaf.pitch == 0 && leaf.roll == 0);
    // The spawn stores the sector builder into item 900's collision and scar
    // callbacks, not its draw: the fragment draws by item 900's own render tag
    // (none here: every section at its own origin) and collides through the
    // palm state (D-ITEMDEF-20) [orig: Projectile_SpawnFromTile @0x53C3B0 /
    // @0x53C3C6].
    CHECK(leaf.palm_fragment && !leaf.palm_sections);
    CHECK(item_hidden_sections(leaf) == 0);
    CHECK(item_section_render_position(w, leaf).z == 7);
    CHECK(palm_sector_hidden_sections(leaf) == 0x3B);
    CHECK(palm_sector_position(w, leaf).z == 3);
    CHECK(w.out.entity_events.size() == 1 && std::get<ItemStateEvent>(w.out.entity_events[0]).section == 5);
    CHECK(w.out.destruction.sounds.size() == 1);
    const auto state_b = w.prng16_b_state;
    destruction_notify_item_damage(w, e, 1, {2, 1}); // Sends again; no second transition/draw.
    CHECK(w.prng16_b_state == state_b && spawner.pieces.size() == 1);
    CHECK(w.out.entity_events.size() == 2);
    destruction_notify_item_damage(w, e, 1, {3, 101});
    CHECK(e.palm_state == 1 && spawner.pieces.size() == 2);
    CHECK(w.registry.get(spawner.pieces[1])->palm_state == 32);
    CHECK(w.registry.get(spawner.pieces[1])->position.z == 5);
    CHECK(item_hidden_sections(e) == 0x36);
    // The psec draw hides the sector builder's sections alone: neither the
    // section mask nor a husk's piece mask joins them (D-ITEMDEF-20)
    // [orig: BoneCallback_psec_World @0x53C130].
    e.section_mask = 0x1;
    e.engine_flags |= kEntityFlagHusk;
    e.spawned_piece_mask = 0x8;
    CHECK(item_hidden_sections(e) == 0x36);
    e.section_mask = e.spawned_piece_mask = 0;
    e.engine_flags &= ~kEntityFlagHusk;
    leaf.position = {8,16,1}; leaf.veh.vel_x = 4096; leaf.veh.vel_y = -4096;
    leaf.veh.slide_z = -65536;
    tick_item_death_motion(w, leaf, nullptr, 0, w.out.destruction);
    CHECK(leaf.position.x == 8 && leaf.position.z == .25f);
    CHECK(leaf.veh.vel_x == 1024 && leaf.veh.vel_y == -1024 && leaf.veh.slide_z == 16384);
    const float yaw = leaf.yaw;
    tick_item_death_motion(w, leaf, nullptr, 0, w.out.destruction);
    CHECK(leaf.position.x == 8.015625f && leaf.position.z == .5f);
    CHECK(leaf.veh.slide_z == 16384 - 167 && leaf.yaw != yaw && leaf.pitch == 0);
    CHECK(leaf.saved_live_pos[2] == 16384);
    e.health = 0;
    destruction_notify_item_damage(w, e, 2);
    CHECK((e.engine_flags & 6) == 6 && !e.alive);
    CHECK(std::get<ItemStateEvent>(w.out.entity_events.back()).section == -1 && e.class_think_ticks == 32);
    destruction_notify_item_damage(w, e, 1, {0, 200});
    CHECK(e.class_think_ticks == 1024 && spawner.pieces.size() == 2);

    w.rules.logic_authority = false;
    const auto client_h = spawn(w, 2, 22, ItemDeathClass::kPalm);
    w.tables.item_death_traits.set(22, *w.tables.item_death_traits.get(21));
    auto &client = *w.registry.get(client_h); client.position.z = 3;
    apply_item_state_event(w, client, 0);
    CHECK(client.palm_state == 1 && client.health == 0 && (client.engine_flags & 6) == 0);
    // The event writes +0x270 but never selects its draw: under a render tag
    // with no section row every section still draws.
    // [orig: WeaponOverlay_HandleDamage @0x53C4C0; BoneCallback_gnrc_World @0x4E2860]
    CHECK(!client.palm_sections && item_hidden_sections(client) == 0);
    CHECK(w.registry.get(spawner.pieces.back())->palm_state == 16);
    const auto count = spawner.pieces.size();
    apply_item_state_event(w, client, -1);
    CHECK((client.engine_flags & 6) == 6 && spawner.pieces.size() == count);
    return 0;
}
int test_tower_sections() {
    auto heap = std::make_unique<World>(); auto &w = *heap;
    w.registry.configure_pool(2, 16); w.rules.mp_session = true;
    TestPieceSpawner spawner(w); w.item_piece_spawner = &spawner;
    const auto h = spawn(w, 2, 50, ItemDeathClass::kTower);
    auto &e = *w.registry.get(h); e.position = {1,2,3}; e.yaw = 90;
    auto *t = w.tables.item_death_traits.get_mutable(50);
    t->model_loaded = true; t->husk_sub_part_count = 4;
    t->model_section_origins_q16 = {{{0,0,0}},{{0,0,4*65536}},{{0,0,8*65536}},{{0,0,12*65536}}};
    t->husk_section_origins_q16 = t->model_section_origins_q16;
    t->model_section_heights_q16.assign(4, 4*65536);
    t->particlefinale = "Dust"; t->particledeath = "Smoke";
    destruction_notify_item_damage(w, e, 1, {1,14});
    CHECK(e.health == 100 && e.item_section_damage[1] == 14 && spawner.pieces.empty());
    CHECK(e.item_section_damage.bytes.size() == 2); // the bank grows to the written section
    destruction_notify_item_damage(w, e, 1, {2,15});
    CHECK(spawner.pieces.size() == 1 && e.spawned_piece_mask == 12);
    auto &piece = *w.registry.get(spawner.pieces[0]);
    CHECK(piece.position.z == 11 && piece.spawned_piece_mask == 3);
    CHECK(piece.section_pitch_rate == 0x16C16C0 && piece.engine_flags == 6);
    CHECK((piece.death_anim_state >> 8) == 2 && piece.health == 20);
    CHECK(item_section_render_position(w, piece).z == 3);
    CHECK(e.health == 100 && (e.engine_flags & 6) == 4);
    CHECK(item_hidden_sections(e) == 12);
    destruction_notify_item_damage(w, e, 1, {1,1});
    CHECK(spawner.pieces.size() == 2 && e.spawned_piece_mask == 14);
    CHECK(w.registry.get(spawner.pieces[1])->section_pitch_rate == 190887424);
    destruction_notify_item_damage(w, e, 1, {3,14}); // completion uses >=14, spawn uses >14
    CHECK(e.health == 0 && (e.engine_flags & 6) == 6 && !e.alive);
    CHECK(w.out.entity_events.size() == 1 && std::get<ItemStateEvent>(w.out.entity_events[0]).section == -1);
    CHECK(e.class_think_ticks == 1024 && spawner.pieces.size() == 2);
    destruction_notify_item_damage(w, piece, 0);
    CHECK(piece.class_think_ticks == 0x1000000);
    // The clone is marked as one; the palm fragment is not.
    // [orig: Entity_SpawnSectionEntity @0x440365]
    CHECK(piece.section_clone && piece.item_section_piece);
    piece.veh.slide_z = 65536;
    tick_item_death_motion(w, piece, nullptr, 0, w.out.destruction);
    CHECK(piece.position.z == 12 && piece.veh.slide_z == 32768);
    piece.position.z = .25f; piece.pitch = 120; piece.veh.slide_z = 0;
    piece.death_anim_state &= ~3;
    tick_item_death_motion(w, piece, nullptr, 0, w.out.destruction);
    CHECK(piece.death_motion == DeathMotionMode::SectionSettled);
    CHECK(piece.section_pitch_accel == 8*65536 && piece.section_pitch_rate == 4*65536);
    tick_item_death_motion(w, piece, nullptr, 0, w.out.destruction);
    CHECK(piece.death_motion == DeathMotionMode::SectionSettled && piece.section_pitch_rate == 8*65536);
    tick_item_death_motion(w, piece, nullptr, 0, w.out.destruction);
    CHECK(piece.death_motion == DeathMotionMode::None);
    // Every contact/settle effect is the def+0x412 particledeath handle, never
    // particlefinale: bottom contact, top contact, the settle loop and the two
    // settled-clock visits above.
    size_t smoke = 0;
    for (const auto &effect : w.out.destruction.effects) {
        CHECK(effect.effect != "Dust");
        smoke += effect.effect == "Smoke";
    }
    CHECK(smoke == 5);
    w.rules.logic_authority = false;
    const auto client_h = spawn(w, 2, 51, ItemDeathClass::kTower);
    w.tables.item_death_traits.set(51, *w.tables.item_death_traits.get(50));
    auto &client = *w.registry.get(client_h);
    client.item_section_damage[2] = 250; client.health = 77;
    destruction_notify_item_damage(w, client, 1, {2,10});
    CHECK(client.item_section_damage[2] == 4 && client.health == 77);
    CHECK(client.class_think_ticks == 32 && spawner.pieces.size() == 2);
    client.engine_flags = kEntityFlagDead;
    destruction_notify_item_damage(w, client, 1, {2,11}); // client hits precede Dead guard
    CHECK(client.item_section_damage[2] == 15 && spawner.pieces.size() == 3);
    // Both legs bound the section by the def's int8 section count: a section
    // past it neither grows the bank nor accumulates (D-ITEM-7).
    destruction_notify_item_damage(w, client, 1, {5,20});
    CHECK(client.item_section_damage.read(5) == 0 && client.item_section_damage.bytes.size() == 3);
    CHECK(client.class_think_ticks == 32 && spawner.pieces.size() == 3);
    return 0;
}

// A section piece is the memset template clone: the parent def's door, squib
// and sway selectors never reach it, only the piece's own callbacks and models.
int test_piece_spawn_clears_class_selectors() {
    const char definitions[] = "begin Tower\r\n id 105050\r\n type building\r\n ai_function door\r\n"
            " move_function squib\r\n render_function tree\r\n num_doors 2\r\n first_door 1\r\n"
            " open_rate 2\r\n max_angle 90\r\n hp 100\r\n end\r\n";
    opennova::def::DefItemsFile items{};
    CHECK(opennova::def::def_parse_items_memory(
            reinterpret_cast<const unsigned char *>(definitions), sizeof(definitions)-1, &items) == 0);
    opennova::mission::MissionKernel kernel;
    kernel.open_document(opennova::bms::File{}, "piece", {});
    kernel.set_items_table(&items);
    opennova::mission::KernelBootOptions options;
    options.playable = false;
    options.wac = false;
    options.collision = false;
    options.seat_specs = false;
    options.terrain = false;
    std::string error;
    CHECK(kernel.boot(options, error));
    Entity seed;
    seed.kind = EntityKind::Building; seed.item_id = 5050; seed.item_type = 5;
    seed.health = 20; seed.engine_flags = 6; seed.alive = false;
    seed.item_section_piece = true;
    seed.death_motion = DeathMotionMode::SectionFalling;
    const auto handle = kernel.spawn_item_piece(seed);
    CHECK(handle.valid());
    const Entity &piece = *kernel.world.registry.get(handle);
    CHECK(piece.has_item_def && piece.item_type == 5 && piece.health == 20);
    CHECK(piece.engine_flags == 6 && !piece.alive);
    CHECK(piece.death_motion == DeathMotionMode::SectionFalling);
    CHECK(!piece.door_event && !piece.door_motion && piece.door_count == 0);
    CHECK(!piece.squib.motor && !piece.render_sway);
    opennova::def::def_free_items(&items);
    return 0;
}

// A section clone runs the null event row whatever its def: a tower whose
// render tag is the psec row (the clone draws the palm state through it,
// palm_sections) takes no damage on its clones, where the tower row would
// accumulate and spawn more sections.
// [orig: Entity_SpawnSectionEntity @0x440365 -> sub_406FF0; the def's world
//  callback overriding the husk's Render_SectorEntity @0x5C431B]
int test_psec_tower_section_clone_stays_inert() {
    const char definitions[] = "begin Tower\r\n id 105051\r\n type building\r\n ai_function towr\r\n"
            " render_function psec\r\n husk_sub_parts 4\r\n hp 100\r\n end\r\n";
    opennova::def::DefItemsFile items{};
    CHECK(opennova::def::def_parse_items_memory(
            reinterpret_cast<const unsigned char *>(definitions), sizeof(definitions)-1, &items) == 0);
    opennova::mission::MissionKernel kernel;
    kernel.open_document(opennova::bms::File{}, "piece", {});
    kernel.set_items_table(&items);
    opennova::mission::KernelBootOptions options;
    options.playable = false;
    options.wac = false;
    options.collision = false;
    options.seat_specs = false;
    options.terrain = false;
    std::string error;
    CHECK(kernel.boot(options, error));
    Entity seed;
    seed.kind = EntityKind::Building; seed.item_id = 5051; seed.item_type = 5;
    seed.health = 20; seed.engine_flags = 6; seed.alive = false;
    seed.item_section_piece = true;
    seed.section_clone = true; // as spawn_tower_section seeds it
    seed.death_motion = DeathMotionMode::SectionFalling;
    const auto handle = kernel.spawn_item_piece(seed);
    CHECK(handle.valid());
    Entity *piece = kernel.world.registry.get(handle);
    CHECK(piece != nullptr && piece->section_clone && piece->palm_sections);
    const ItemDeathTraits *traits = kernel.world.tables.item_death_traits.get(5051);
    CHECK(traits != nullptr && traits->death_class == ItemDeathClass::kTower);
    if (piece != nullptr) {
        destruction_notify_item_damage(kernel.world, *piece, 1, {2, 20});
        CHECK(piece->class_think_ticks == 0x1000000);
        CHECK(piece->item_section_damage.read(2) == 0);
        CHECK(kernel.world.out.entity_events.empty());
    }
    opennova::def::def_free_items(&items);
    return 0;
}

// The fade's math alone, as the editor's model preview reads it (DI-10): the authored duration and stagger
// (0 takes 50 and 25), each phase clamped, the overall share unclamped.
int test_destroy_fade_phases() {
    const int32_t timing[3] = {10, 50, 25};
    DestroyFade fade = destroy_fade_phases(50, timing);
    CHECK(fade.phases_q16 == (std::array<int32_t,6>{21845,65536,32768,0,0,0}));
    CHECK(std::fabs(fade.progress - 50.0/150.0) < 1e-12);
    const int32_t defaults[3] = {0, 0, 0};
    fade = destroy_fade_phases(100, defaults);
    CHECK(fade.phases_q16 == (std::array<int32_t,6>{43690,65536,65536,65536,32768,0}));
    fade = destroy_fade_phases(-5, defaults);
    CHECK(fade.phases_q16 == (std::array<int32_t,6>{}) && fade.progress < 0);
    fade = destroy_fade_phases(400, defaults);
    CHECK(fade.phases_q16 == (std::array<int32_t,6>{65536,65536,65536,65536,65536,65536}) && fade.progress > 1);
    return 0;
}

int test_destroy_phases_and_ambient() {
    auto heap = std::make_unique<World>();
    auto &w = *heap;
    w.registry.configure_pool(2, 8);
    auto &e = *w.registry.get(spawn(w, 2, 70, ItemDeathClass::kGnrl));
    auto *traits = w.tables.item_death_traits.get_mutable(70);
    traits->destroy_timing_ticks[0] = 10;
    traits->destroy_timing_ticks[1] = 50;
    traits->destroy_timing_ticks[2] = 25;
    e.destroy_timer = 10;
    e.death_tick = 100;
    e.engine_flags = kEntityFlagDead | kEntityFlagHusk;
    w.logic_tick = 109;
    tick_item_death_motion(w,e,nullptr,0,w.out.destruction);
    CHECK(e.destroy_phases_q16 == (std::array<int32_t,6>{}));
    w.logic_tick = 110;
    tick_item_death_motion(w,e,nullptr,0,w.out.destruction);
    CHECK(e.death_tick == 110 && e.destroy_timer == 0);
    w.logic_tick = 160;
    tick_item_death_motion(w,e,nullptr,0,w.out.destruction);
    CHECK(e.destroy_phases_q16 == (std::array<int32_t,6>{21845,65536,32768,0,0,0}));
    w.logic_tick = 260;
    tick_item_death_motion(w,e,nullptr,0,w.out.destruction);
    CHECK(e.destroy_phases_q16 == (std::array<int32_t,6>{65536,65536,65536,65536,65536,65536}));
    e.engine_flags = 0;
    e.death_motion = DeathMotionMode::BuildingEffects;
    e.item_type = 5;
    e.position = {3,4,5};
    e.bbox_center = {0,0,2};
    w.weather.tod_fixed24 = uint32_t(12*65536) << 8;
    traits->regional_loops[1] = "Day";
    tick_item_death_motion(w,e,nullptr,0,w.out.destruction);
    CHECK(e.destroy_phases_q16 == (std::array<int32_t,6>{}));
    CHECK(w.out.sound_emitters.size() == 1);
    CHECK(w.out.sound_emitters[0].set_name == "Day");
    CHECK(w.out.sound_emitters[0].pos.z == 7);
    CHECK(w.out.sound_emitters[0].lifetime_ticks == 31);
    CHECK(w.out.sound_emitters[0].volume_q8_8 == 65534);
    CHECK(w.out.sound_emitters[0].pitch_q16 == 65536);
    return 0;
}
int test_class_scoring_and_explosion_draws() {
    auto heap = std::make_unique<World>();
    auto &w = *heap;
    w.registry.configure_pool(0,8);
    w.registry.configure_pool(1,8);
    w.rules.mp_session = true;
    MatchRules rules;
    rules.game_type = 0x90002; // Search and Destroy
    w.match.configure(rules);
    Entity player;
    player.kind = EntityKind::Organic;
    player.team = 1;
    const auto attacker = w.registry.spawn(0,player);
    const auto driver = w.registry.spawn(0,player);
    const auto parent_driver = w.registry.spawn(0,player);
    w.match.upsert_player({attacker,0,"A"});
    w.match.upsert_player({driver,1,"B"});
    w.match.upsert_player({parent_driver,2,"C"});
    w.registry.get(attacker)->primary_occupant = driver;
    w.registry.get(driver)->primary_occupant = parent_driver;
    auto &target = *w.registry.get(spawn(w,1,71,ItemDeathClass::kGnrl));
    target.item_attrib = 0x8000;
    target.health = 0;
    target.last_attacker = attacker;
    destruction_notify_item_damage(w,target,1);
    CHECK(target.objective_death_scored);
    const int points = w.match.player(attacker)->stats[MatchStats::kPoints];
    CHECK(points > 0);
    CHECK(w.match.player(driver)->stats[MatchStats::kPoints] == points/2);
    // The second link takes event 11's direct bonus>>2 AND the nested
    // (bonus>>1)>>1 from the first link's event-28 recursion
    // [orig: @0x52faff + CPlayerStats_RecordEvent case 28 @0x52cb50].
    CHECK(w.match.player(parent_driver)->stats[MatchStats::kPoints] == ((points >> 1) >> 1) + (points >> 2));
    w.match.record_death(w,target.handle,attacker);
    CHECK(w.match.player(attacker)->stats[MatchStats::kTargetsDestroyed] == 1);
    CHECK(w.out.entity_events.size() == 1);
    CHECK(std::get<ItemStateEvent>(w.out.entity_events[0]).handle == target.handle.packed);
    w.rules.mp_session = false;
    const uint32_t seed = w.throwables.fan_prng_state;
    for (int i = 0; i < 24; ++i) w.throwables.fan_prng();
    const uint32_t expected = w.throwables.fan_prng_state;
    w.throwables.fan_prng_state = seed;
    spawn_item_explosion(w,nullptr,{65536,2*65536,3*65536},0,60);
    CHECK(w.throwables.fan_prng_state == expected);
    CHECK(w.out.destruction.effects.back().effect == "Effect_AirExp");
    CHECK(w.out.destruction.effects.back().pos.z == 3);
    return 0;
}

int main() {
    if (test_destroy_fade_phases() || test_destroy_phases_and_ambient() ||
            test_class_scoring_and_explosion_draws()) return 1;
    if (test_tower_sections() != 0) return 1;
    if (test_piece_spawn_clears_class_selectors() != 0) return 1;
    if (test_palm_sections() != 0) return 1;
    if (test_psec_tower_section_clone_stays_inert() != 0) return 1;
    if (test_emitter_lifetime() != 0) return 1;
    if (test_crane_pair_and_fade() != 0) return 1;
    if (test_building_collapse() != 0) return 1;
    if (test_flag_callback() != 0) return 1;
    if (test_regional_sound() != 0) return 1;
    auto heap = std::make_unique<World>();
    auto &w = *heap;
    for (int pool = 0; pool <= 3; ++pool) w.registry.configure_pool(pool, 8);
    const auto p1 = spawn(w, 1, 1, ItemDeathClass::kElevator);
    const auto p2 = spawn(w, 2, 2, ItemDeathClass::kElevator);
    const auto p3 = spawn(w, 3, 3, ItemDeathClass::kElevator);
    for (uint32_t t = 0; t <= 128; ++t) {
        w.logic_tick = t;
        for (int p = 1; p <= 3; ++p) step_item_pool(w, p);
        if (t == 0) {
            CHECK(w.registry.get(p1)->class_think_ticks == 61);
            CHECK(w.registry.get(p2)->class_think_ticks == 62);
            CHECK(w.registry.get(p3)->class_think_ticks == 62);
        }
        if (t == 64) {
            CHECK(w.registry.get(p2)->class_think_ticks == -2);
            CHECK(w.registry.get(p3)->class_think_ticks == -2);
        }
        if (t == 72) CHECK(w.registry.get(p2)->class_think_ticks == 62);
    }
    CHECK(w.registry.get(p3)->class_think_ticks == 62);
    const auto barrel = spawn(w, 1, 4, ItemDeathClass::kBarrel);
    w.registry.get(barrel)->health = 0;
    destruction_notify_item_damage(w, *w.registry.get(barrel), 1);
    CHECK(w.registry.get(barrel)->class_think_ticks == 10);
    CHECK((w.registry.get(barrel)->engine_flags & kEntityFlagHusk) == 0);
    for (uint32_t t = 0; t <= 10; ++t) {
        w.logic_tick = t;
        step_item_pool(w, 1);
        if (t < 10) CHECK(w.registry.get(barrel) != nullptr);
    }
    CHECK(w.registry.get(barrel) == nullptr);
    CHECK(w.out.destruction.effects.size() == 1);
    CHECK(w.out.destruction.effects[0].effect == "Effect_AirExp");

    const auto building = spawn(w, 2, 5, ItemDeathClass::kBuilding);
    auto &b = *w.registry.get(building);
    auto *bt = w.tables.item_death_traits.get_mutable(5);
    bt->model_loaded = true;
    bt->model_radius_xy_q16 = 2 * 65536;
    bt->model_radius_z_q16 = 65536;
    Entity victim;
    victim.has_item_def = true;
    victim.item_type = 3;
    victim.position = {2, 2, 1};
    const auto unnumbered = w.registry.spawn(0, victim); // +0x1C ItemTypeIndex zero: skipped
    victim.item_id = 9;
    victim.item_type_index = 9;
    const auto person = w.registry.spawn(0, victim);
    victim.damage_state = 1;
    const auto protected_person = w.registry.spawn(0, victim);
    const auto item = w.registry.spawn(1, victim); // pool 1 ignores damage_state
    victim.item_type = 5;
    const auto excluded_building = w.registry.spawn(2, victim);
    victim.item_type = 4;
    const auto crushable_building = w.registry.spawn(2, victim); // pool 2 ignores damage_state
    b.health = 0;
    destruction_notify_item_damage(w, b, 1);
    CHECK(w.registry.get(unnumbered)->health == 100);
    CHECK(w.registry.get(person)->health == 0);
    CHECK(w.registry.get(protected_person)->health == 100);
    CHECK(w.registry.get(item)->health == 0);
    CHECK(w.registry.get(excluded_building)->health == 100);
    CHECK(w.registry.get(crushable_building)->health == 0);
    CHECK((b.engine_flags & 6) == 6 && b.class_think_ticks == 1024);

    const auto target = spawn(w, 2, 6, ItemDeathClass::kTarget);
    auto &door = *w.registry.get(target);
    door.door_event = true;
    door.door_count = 2;
    door.door_first_bone = 1;
    w.doors.initialize(door, 10, 100);
    destruction_notify_item_damage(w, door, 1, {1, 1});
    CHECK(w.doors.slot(door, 0)->state == 1);
    CHECK(w.doors.slot(door, 1)->state == 0);
    CHECK(door.door_touch_mask == 1 && door.class_think_ticks == 1920);
    destruction_notify_item_damage(w, door, 1, {0, 100});
    CHECK(w.doors.slot(door, 0)->state == 1);

    w.rules.logic_authority = false;
    const auto remote = spawn(w, 1, 7, ItemDeathClass::kBarrel);
    w.registry.get(remote)->health = 0;
    destruction_notify_item_damage(w, *w.registry.get(remote), 4);
    CHECK(w.registry.get(remote)->engine_flags == 0);
    const auto lift = w.registry.get(p1);
    lift->health = 0;
    destruction_notify_item_damage(w, *lift, 4);
    CHECK(lift->engine_flags == 0 && lift->class_think_ticks == 62);
    return 0;
}
