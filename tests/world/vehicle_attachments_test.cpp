// Gunner attachments: the class init's same-refNum pool-1 peer list, the
// greedy 'agun' userpoint assignment, the per-tick follow the installed +0x1C4
// callback runs after the saved motor, and the vehicle DYING enter's kill loop
// over the same list.
// [orig: Entity_SetupGunnerAttachments @0x468100; Entity_UpdateAttachedChildren
//  @0x45D550; AI_TransitionToDeath_GroundVehicle @0x467B6E..0x467BBB;
//  the `Parent` gate Entity_InitVehicleAIFromDef @0x46895A..0x468964]
#include <runtime/devtools/tick_profile.h>
#include <runtime/world/ai.h>
#include <runtime/world/entity.h>
#include <runtime/world/vehicle_motor.h>
#include <runtime/world/world.h>

#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

using namespace opennova::world;

static int failures = 0;
#define CHECK(c) \
    do { if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

namespace {

// Two 'agun' points two units fore and aft of the hull origin (model-local
// 16.16); the hull faces mission yaw 90 (engine heading 0), so the placement
// matrix is the identity rotation and world = origin + local.
VehicleTraits parent_traits(bool attrib_parent) {
    VehicleTraits t;
    t.physics = 1;
    t.player_speed = 64 * 293;
    t.acceleration = 10 * 4;
    t.deceleration = 20 * 4;
    t.turn_rate = 45 * 192426;
    t.turn_rate2 = 30 * 192426;
    t.player_control = true;
    t.attrib_parent = attrib_parent;
    VehicleEffectPoint fore;
    fore.position[0] = 2 << 16;
    VehicleEffectPoint aft;
    aft.position[0] = -(2 << 16);
    t.agun_points = { fore, aft };
    return t;
}

struct Rig {
    std::unique_ptr<World> wp = std::make_unique<World>();
    World &w = *wp;
    EntityHandle veh_h;
    explicit Rig(bool attrib_parent = true, uint8_t ref_num = 5) {
        w.registry.configure_pool(0, 8);
        w.registry.configure_pool(1, 32);
        w.ai.is_authority = true;
        Entity veh;
        veh.kind = EntityKind::Item;
        veh.has_item_def = true;
        veh.item_id = 1294;
        veh.ref_num = ref_num;
        veh.position = {100.0f, 50.0f, 5.0f};
        veh.yaw = 90;
        veh.health = 2000;
        veh.health_max = 2000;
        veh.alive = true;
        veh_h = w.registry.spawn(1, veh);
        w.vehicles.traits.set(1294, parent_traits(attrib_parent));
        AiEntity &brain = *w.ai.at(w.ai.attach(veh_h));
        brain.health = 2000;
        brain.brain.f[AiBrain::kCurState] = kAiGroundPretty;
        brain.brain.f[AiBrain::kPendState] = kAiGroundPretty;
        brain.brain.f[AiBrain::kStep] = 16;
    }
    Entity &veh() { return *w.registry.get(veh_h); }
    AiBrain &brain() { return w.ai.for_handle(veh_h)->brain; }
    EntityHandle spawn_peer(uint8_t ref_num, const Vec3 &pos, int32_t item_id = 1800) {
        Entity e;
        e.kind = EntityKind::Item;
        e.has_item_def = true;
        e.item_id = item_id;
        e.ref_num = ref_num;
        e.position = pos;
        e.health = 500;
        e.health_max = 500;
        e.alive = true;
        return w.registry.spawn(1, e);
    }
    int32_t slot_bone(int slot) { return brain().f[AiBrain::kAttachSlots + 2 * slot]; }
    EntityHandle slot_child(int slot) {
        const int32_t word = brain().f[AiBrain::kAttachSlots + 2 * slot + 1];
        return word > 0 ? EntityHandle{static_cast<uint16_t>(word - 1)} : EntityHandle{};
    }
};

bool close_to(float a, float b, float tol = 1e-3f) { return std::fabs(a - b) <= tol; }

// The peer walk: every OTHER pool-1 entity with the same nonzero refNum, in
// slot order, then the greedy nearest-point assignment that consumes each
// point once; a child with nothing left keeps bone 0.
void test_peer_collection_and_greedy_assignment() {
    Rig r;
    const EntityHandle a = r.spawn_peer(5, {98.5f, 50.0f, 5.0f});  // nearest the aft point
    const EntityHandle b = r.spawn_peer(5, {101.5f, 50.0f, 5.0f}); // nearest the fore point
    const EntityHandle c = r.spawn_peer(5, {300.0f, 300.0f, 5.0f}); // nothing left
    r.spawn_peer(0, {100.0f, 50.0f, 5.0f});                         // refNum 0: never a peer
    r.spawn_peer(6, {100.0f, 50.0f, 5.0f});                         // another refNum
    r.w.vehicles.setup_gunner_attachments(r.veh());
    CHECK(r.brain().f[AiBrain::kAttachCount] == 3);
    CHECK(r.slot_child(0) == a && r.slot_child(1) == b && r.slot_child(2) == c);
    CHECK(r.slot_bone(0) == 2); // the aft point (index 1) -> word 2
    CHECK(r.slot_bone(1) == 1); // the fore point (index 0) -> word 1
    CHECK(r.slot_bone(2) == 0); // both points consumed

    // A vehicle whose own refNum is zero collects nothing.
    Rig zero(true, 0);
    zero.spawn_peer(0, {100.0f, 50.0f, 5.0f});
    zero.w.vehicles.setup_gunner_attachments(zero.veh());
    CHECK(zero.brain().f[AiBrain::kAttachCount] == 0);

    // The def gate: without the `Parent` attrib nothing is collected.
    Rig gated(false);
    gated.spawn_peer(5, {98.5f, 50.0f, 5.0f});
    gated.w.vehicles.setup_gunner_attachments(gated.veh());
    CHECK(gated.brain().f[AiBrain::kAttachCount] == 0);

    // Sixteen slots at most.
    Rig many(true, 7);
    for (int i = 0; i < 18; ++i)
        many.spawn_peer(7, {100.0f + float(i), 50.0f, 5.0f});
    many.w.vehicles.setup_gunner_attachments(many.veh());
    CHECK(many.brain().f[AiBrain::kAttachCount] == 16);
}

// The follow: an assigned child adopts the parent's eulers and the point's
// world position every call, and the parent's six velocity words unless it is
// dead or flagged dead/husk; an unassigned child stays put.
void test_per_tick_follow() {
    Rig r;
    const EntityHandle a = r.spawn_peer(5, {98.5f, 50.0f, 5.0f});
    const EntityHandle b = r.spawn_peer(5, {101.5f, 50.0f, 5.0f});
    const EntityHandle c = r.spawn_peer(5, {300.0f, 300.0f, 5.0f});
    r.w.vehicles.setup_gunner_attachments(r.veh());

    Entity &veh = r.veh();
    veh.position = {110.0f, 50.0f, 5.0f};
    veh.veh.vel_x = 123;
    veh.veh.vel_y = -45;
    veh.veh.slide_z = 7;
    veh.veh.wheel_rate_bam = 99;
    veh.veh.air_pitch_rate = 5;
    veh.veh.air_roll_rate = -6;
    r.w.registry.get(b)->health = 0; // a dead child rides along with zero velocity
    r.w.vehicles.update_attached_children(veh);

    const Entity &ea = *r.w.registry.get(a);
    CHECK(close_to(ea.position.x, 108.0f) && close_to(ea.position.y, 50.0f) && close_to(ea.position.z, 5.0f));
    CHECK(ea.yaw == 90 && ea.pitch == 0 && ea.roll == 0);
    CHECK(ea.veh.vel_x == 123 && ea.veh.vel_y == -45 && ea.veh.slide_z == 7);
    CHECK(ea.veh.wheel_rate_bam == 99 && ea.veh.air_pitch_rate == 5 && ea.veh.air_roll_rate == -6);
    const Entity &eb = *r.w.registry.get(b);
    CHECK(close_to(eb.position.x, 112.0f) && close_to(eb.position.y, 50.0f));
    CHECK(eb.veh.vel_x == 0 && eb.veh.vel_y == 0 && eb.veh.slide_z == 0);
    CHECK(eb.veh.wheel_rate_bam == 0 && eb.veh.air_pitch_rate == 0 && eb.veh.air_roll_rate == 0);
    const Entity &ec = *r.w.registry.get(c);
    CHECK(close_to(ec.position.x, 300.0f) && close_to(ec.position.y, 300.0f));

    // A husk-flagged live child also gets zero velocity.
    r.w.registry.get(b)->health = 100;
    r.w.registry.get(b)->engine_flags |= kEntityFlagHusk;
    r.w.vehicles.update_attached_children(veh);
    CHECK(r.w.registry.get(b)->veh.vel_x == 0);

    // A brain-carrying child sees the same words through its AiEntity mirrors.
    AiEntity &child_brain = *r.w.ai.at(r.w.ai.attach(a));
    r.w.vehicles.update_attached_children(veh);
    CHECK(child_brain.pos[0] == (108 << 16) && child_brain.pos[1] == (50 << 16));
    CHECK(child_brain.vel_x == 123 && child_brain.vel_z == -45);

    // The motor pass chains the follow after the mover: the children keep
    // their hull-relative offsets through an authority tick.
    opennova::devtools::ProfileLap lap(r.w.profile);
    r.w.vehicles.tick_motors(true, lap);
    CHECK(close_to(r.w.registry.get(a)->position.x - r.veh().position.x, -2.0f, 0.01f));
    CHECK(close_to(r.w.registry.get(b)->position.x - r.veh().position.x, 2.0f, 0.01f));
    CHECK(close_to(r.w.registry.get(a)->position.y - r.veh().position.y, 0.0f, 0.01f));
}

// The vehicle DYING enter kills every live attached child through its class
// death callback (phase 1); the gate is the def's `Parent` byte.
void test_dying_enter_kills_the_attached_list() {
    Rig r;
    const EntityHandle a = r.spawn_peer(5, {98.5f, 50.0f, 5.0f});
    const EntityHandle b = r.spawn_peer(5, {101.5f, 50.0f, 5.0f});
    r.w.vehicles.setup_gunner_attachments(r.veh());
    r.veh().health = 0;
    AiEntity &brain = *r.w.ai.for_handle(r.veh_h);
    brain.health = 0;
    brain.brain.f[AiBrain::kPendState] = 21;
    r.w.ai.apply_transition(brain, r.w);
    CHECK(brain.brain.f[AiBrain::kCurState] == 21);
    CHECK(r.w.registry.get(a)->health == 0 && r.w.registry.get(b)->health == 0);
    CHECK((r.w.registry.get(a)->engine_flags & kEntityFlagHusk) != 0);

    Rig gated(false);
    const EntityHandle g = gated.spawn_peer(5, {98.5f, 50.0f, 5.0f});
    gated.w.vehicles.setup_gunner_attachments(gated.veh());
    gated.veh().health = 0;
    AiEntity &gb = *gated.w.ai.for_handle(gated.veh_h);
    gb.health = 0;
    gb.brain.f[AiBrain::kPendState] = 21;
    gated.w.ai.apply_transition(gb, gated.w);
    CHECK(gated.w.registry.get(g)->health == 500);
}

} // namespace

int main() {
    test_peer_collection_and_greedy_assignment();
    test_per_tick_follow();
    test_dying_enter_kills_the_attached_list();
    if (failures == 0) std::printf("vehicle_attachments_test: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
