// The local player's aim/target acquisition (world/local_player_targeting.cpp)
// [orig: Entity_UpdateInfantryPlayerBody @0x4b4e9b..0x4b5288]: the weapon-view
// ray's scope-zero term, the designator lock storing a mountable gun's
// redirected hull, and the outer loop's catch-up gate on the locked tone.
#include <cstdint>
#include <cstdio>
#include <cstring>

#include <base/io/bam.h>
#include <formats/def/def.h>
#include <runtime/world/ai.h>
#include <runtime/world/angle.h>
#include <runtime/world/collision.h>
#include <runtime/world/entity.h>
#include <runtime/world/local_player.h>
#include <runtime/world/local_player_view.h>
#include <runtime/world/world.h>

using namespace opennova::world;
using namespace opennova::def;

static int failures = 0;
#define CHECK(c)                                                                       \
    do {                                                                               \
        if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
    } while (0)

namespace {

// A world with one live local organic at (10, 20, 3), the shape the body
// updater ticks over; pool 1 holds the dynamic items the aim ray can hit.
struct LocalWorld {
    World w;
    AiSystem &ai = w.ai;
    EntityHandle local;

    LocalWorld() {
        w.registry.configure_pool(0, 8);
        w.registry.configure_pool(1, 8);
        Entity seed;
        seed.kind = EntityKind::Organic;
        seed.item_id = 0x14B9;
        seed.net_id = 1;
        seed.position = {10.0f, 20.0f, 3.0f};
        seed.alive = true;
        seed.health = 100;
        local = w.registry.spawn(0, seed);
        w.cached.local_player = local;
    }
    Entity &entity() { return *w.registry.get(local); }
    AiEntity &body() {
        ai.attach(local);
        AiEntity &b = *ai.for_handle(local);
        b.pos[0] = 10 << 16; b.pos[1] = 20 << 16; b.pos[2] = 3 << 16;
        return b;
    }
};

// A vertical q8 CFAC quad centered on the entity pose (x = 0, y = +-1,
// z = 0..2), the projectile suite's knife-leg fixture: a +X ray meets it at
// the entity's X.
CollisionModel vertical_quad_model() {
    CollisionModel m;
    m.face_vertices = {{0, -256, 0}, {0, 256, 0}, {0, 256, 512}, {0, -256, 512}};
    auto face = [&](int a, int b, int c) {
        CollisionFace f;
        f.v[0] = static_cast<int16_t>(a);
        f.v[1] = static_cast<int16_t>(b);
        f.v[2] = static_cast<int16_t>(c);
        f.normal[0] = -16384;
        f.normal[1] = 0;
        f.normal[2] = 0;
        f.axis = 4; // YZ projection
        f.plane_dist = 0;
        f.min[0] = 0;          f.max[0] = 0;
        f.min[1] = -0x10000;   f.max[1] = 0x10000;
        f.min[2] = 0;          f.max[2] = 0x20000;
        f.material = 1;
        m.faces.push_back(f);
    };
    face(0, 1, 2);
    face(0, 2, 3);
    m.sections.assign(1, {});
    m.sections[0].face_start = 0;
    m.sections[0].face_count = 2;
    m.sections[0].face_vertex_start = 0;
    m.sections[0].face_vertex_count = 4;
    return m;
}

// The heat-seeker lock fixture the cadence test uses: a LOCK def whose ammo
// heat-seeks, and one hot enemy vehicle 100 u down +X.
EntityHandle seed_heat_lock(LocalWorld &lw, LocalPlayer &local) {
    local.weapon.active = true;
    local.weapon.def_name = "LOCK";
    std::strcpy(local.weapon.def.soundlockedtone, "LOCKED");
    lw.w.tables.weapons.entries.resize(2);
    auto &def = lw.w.tables.weapons.entries[1];
    def.name = "LOCK"; def.valid = true; def.ammo_index = 1;
    lw.w.tables.ammo.entries.resize(2);
    auto &ammo = lw.w.tables.ammo.entries[1];
    ammo.valid = true; ammo.heat_det_range = 3000; ammo.boresight_maxang = 11930464 * 20;
    Entity target;
    target.item_id = 2; target.item_type = 1; target.health = 100; target.alive = true;
    target.team = 2; target.heat_sig = 500; target.position = {110, 20, 3};
    lw.w.out.fire_sounds.set_listener({10, 20, 3});
    return lw.w.registry.spawn(0, target);
}

// The scope-zero elevation comes back OUT of the weapon-view ray while the
// weapon can fire, and drops when it cannot.
// [orig: Player_CanFireWeapon @0x4b4ea7 -> offsetY = -MountSlot+4
//  @0x4b4eb0..0x4b4eb9, 0 when it cannot fire @0x4b4ebd;
//  Entity_BuildCameraFromWeaponView pitch += offsetY @0x4b0f56]
void test_weapon_view_ray_takes_the_scope_zero_back_out() {
    LocalWorld lw;
    CollisionWorld collision;
    lw.w.collision = &collision;
    AiEntity &body = lw.body();
    body.pitch = bam_from_degrees_wrapped(10.0);
    body.inf.recoil_pitch = bam_from_degrees_wrapped(1.0);
    LocalPlayer local(lw.w);
    local.weapon.active = true;
    local.weapon.def.flags = DEF_WEAPON_FLAG_FORCESCOPED; // the CanFire pin
    local.weapon.slot.zero_pitch = bam_from_degrees_wrapped(4.0);
    CHECK(local.local_player_can_fire());
    const int32_t origin[3] = {body.pos[0], body.pos[1], body.pos[2]};
    const int32_t forward[3] = {1000 << 16, 0, 0};
    const auto endpoint_for = [&](int32_t pitch, int32_t out[3]) {
        collision_matrix_from_euler(body.heading, pitch, body.roll, origin)
                .transform_point(forward, out);
    };
    const int32_t raw_pitch = opennova::io::bam_add(body.pitch, body.inf.recoil_pitch);
    int32_t zeroed[3], raw[3];
    endpoint_for(opennova::io::bam_sub(raw_pitch, local.weapon.slot.zero_pitch), zeroed);
    endpoint_for(raw_pitch, raw);
    CHECK(zeroed[2] != raw[2]);
    lw.w.logic_tick = 16;
    local.update_aim_target();
    CHECK(body.inf.aim_point[0] == zeroed[0] && body.inf.aim_point[2] == zeroed[2]);
    // No fire verdict: offsetY = 0 and the ray follows the raw pitch.
    local.weapon.def.flags = 0;
    CHECK(!local.local_player_can_fire());
    lw.w.logic_tick = 32;
    local.update_aim_target();
    CHECK(body.inf.aim_point[0] == raw[0] && body.inf.aim_point[2] == raw[2]);
}

// The designator lock on a mountable gun stores the gun's REDIRECTED hull,
// not the gun the ray hit.
// [orig: Entity_IsMountableGun @0x434240 -> groundEntity @0x4b51bd..0x4b51c9;
//  the cases 1,2,6-8,10 store @0x4b51e6..0x4b51e9]
void test_designator_lock_stores_the_mountable_guns_hull() {
    LocalWorld lw;
    CollisionWorld collision;
    lw.w.collision = &collision;
    AiEntity &body = lw.body();
    body.team = 1; lw.entity().team = 1;
    LocalPlayer local(lw.w);
    local.weapon.active = true;
    local.weapon.def.flags2 = 0x10; // the designator
    local.weapon.slot.clip = 1;
    Entity hull;
    hull.kind = EntityKind::Item;
    hull.item_id = 3; hull.has_item_def = true; hull.item_type = 1;
    hull.item_attrib = kItemAttribEweap;
    hull.team = 2; hull.health = 100; hull.alive = true;
    hull.position = {110.0f, 40.0f, 3.0f};
    const EntityHandle hull_handle = lw.w.registry.spawn(1, hull);
    Entity gun;
    gun.kind = EntityKind::Item;
    gun.item_id = 4; gun.has_item_def = true; gun.item_type = 5;
    gun.item_attrib = kItemAttribEweap;
    gun.team = 2; gun.health = 100; gun.alive = true;
    gun.position = {110.0f, 20.0f, 2.0f}; // 100 u down the +X ray, the quad spans z 2..4
    gun.bound_radius = 2.0f;
    gun.ground_target = hull_handle;
    const EntityHandle gun_handle = lw.w.registry.spawn(1, gun);
    const int32_t model_id = collision.add_model(vertical_quad_model());
    collision.assign_entity(gun_handle, model_id);
    const int32_t pose[3] = {110 << 16, 20 << 16, 2 << 16};
    CHECK(collision.publish_entity_section_matrices(
            gun_handle, {collision_matrix_from_heading(0, pose)}));
    collision.build_tick_tables(lw.w);
    lw.w.logic_tick = 16;
    local.update_aim_target();
    CHECK(body.inf.head_look_target == gun_handle); // the ray hit the gun
    CHECK(body.inf.combat_target == hull_handle);   // the lock stored its hull
    CHECK(lw.entity().last_fire_target == hull_handle);
    CHECK(body.slot.f[3] == static_cast<int32_t>(hull_handle.packed) + 1);
    CHECK((body.slot.f[2] & 1) != 0);
}

// The locked tone registers only on a frame's last logic tick; the lock
// itself still lands while the bank catches up.
// [orig: dword_24E0E80 @0x4b5229 ahead of SoundEmitter_Register @0x4b5280;
//  Game_MainLoop @0x52ba32..0x52ba3a]
void test_locked_tone_waits_for_the_last_catch_up_tick() {
    LocalWorld lw;
    CollisionWorld collision;
    lw.w.collision = &collision;
    AiEntity &body = lw.body();
    body.team = 1; lw.entity().team = 1;
    LocalPlayer local(lw.w);
    const EntityHandle h = seed_heat_lock(lw, local);
    lw.w.rules.last_tick_of_batch = false;
    lw.w.logic_tick = 16;
    local.update_aim_target();
    CHECK(body.inf.combat_target == h);
    CHECK((body.slot.f[2] & 1) != 0);
    CHECK(lw.w.out.sound_emitters.drain().empty());
    lw.w.rules.last_tick_of_batch = true;
    lw.w.logic_tick = 17;
    local.update_aim_target();
    auto events = lw.w.out.sound_emitters.drain();
    CHECK(events.size() == 1 && events[0].set_name == "LOCKED");
}

} // namespace

int main() {
    test_weapon_view_ray_takes_the_scope_zero_back_out();
    test_designator_lock_stores_the_mountable_guns_hull();
    test_locked_tone_waits_for_the_last_catch_up_tick();
    if (failures == 0) std::printf("local_player_targeting_test: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
